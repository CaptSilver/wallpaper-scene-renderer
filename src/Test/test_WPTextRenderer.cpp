#include <doctest.h>

#include "WPTextRenderer.hpp"
#include "SystemFontFallback.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace wallpaper;

// Helper: feed bytes through WPTextRenderer::DecodeUtf8 and collect the
// decoded codepoints.  The decoder advances `p` byte-by-byte so a malformed
// run can produce multiple REPLACEMENT characters per input string.
static std::vector<uint32_t> decode_all(const char* data, size_t len) {
    std::vector<uint32_t> out;
    const char*           p   = data;
    const char*           end = data + len;
    while (p < end) {
        out.push_back(WPTextRenderer::DecodeUtf8(p, end));
    }
    return out;
}

TEST_SUITE("WPTextRenderer::DecodeUtf8 — well-formed UTF-8") {
    TEST_CASE("ASCII passes through unchanged") {
        const char  s[] = "abc";
        const char* p   = s;
        CHECK(WPTextRenderer::DecodeUtf8(p, s + 3) == uint32_t('a'));
        CHECK(WPTextRenderer::DecodeUtf8(p, s + 3) == uint32_t('b'));
        CHECK(WPTextRenderer::DecodeUtf8(p, s + 3) == uint32_t('c'));
        CHECK(p == s + 3);
    }

    TEST_CASE("2-byte sequence decodes (U+00E9 'é' = C3 A9)") {
        const char  bytes[] = { (char)0xC3, (char)0xA9 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 2) == 0x00E9u);
    }

    TEST_CASE("3-byte sequence decodes (U+4E2D '中' = E4 B8 AD)") {
        const char  bytes[] = { (char)0xE4, (char)0xB8, (char)0xAD };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 3) == 0x4E2Du);
    }

    TEST_CASE("4-byte sequence decodes (U+1F600 '😀' = F0 9F 98 80)") {
        const char  bytes[] = { (char)0xF0, (char)0x9F, (char)0x98, (char)0x80 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 4) == 0x1F600u);
    }
}

TEST_SUITE("WPTextRenderer::DecodeUtf8 — malformed UTF-8 → U+FFFD") {
    TEST_CASE("overlong 2-byte: C0 80 (encodes U+0000 in 2 bytes) rejected") {
        // Overlong forms have historically been used to smuggle ASCII past
        // validators (e.g., null byte as `C0 80`).  Must NOT decode to U+0000.
        const char  bytes[] = { (char)0xC0, (char)0x80 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 2) == WPTextRenderer::kReplacementChar);
    }

    TEST_CASE("overlong 3-byte: E0 80 80 (encodes U+0000 in 3 bytes) rejected") {
        const char  bytes[] = { (char)0xE0, (char)0x80, (char)0x80 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 3) == WPTextRenderer::kReplacementChar);
    }

    TEST_CASE("UTF-16 surrogate half: ED A0 80 (U+D800) rejected") {
        // U+D800–U+DFFF are not valid scalar values; some fonts can crash
        // FT_Load_Char on them.
        const char  bytes[] = { (char)0xED, (char)0xA0, (char)0x80 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 3) == WPTextRenderer::kReplacementChar);
    }

    TEST_CASE("codepoint above Unicode max: F4 90 80 80 (U+110000) rejected") {
        // 4-byte form can carry up to U+1FFFFF; Unicode caps at U+10FFFF.
        const char  bytes[] = { (char)0xF4, (char)0x90, (char)0x80, (char)0x80 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 4) == WPTextRenderer::kReplacementChar);
    }

    TEST_CASE("non-continuation trailing byte: C3 41 ('A' after a 2-byte start)") {
        // Trailing byte must be 10xxxxxx.  An ASCII byte in the trailing
        // slot signals truncation — decoder returns FFFD and leaves `p` on
        // the bad byte so the next call resyncs at that ASCII.
        const char  bytes[] = { (char)0xC3, (char)0x41 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 2) == WPTextRenderer::kReplacementChar);
        // Resync: next call decodes the 'A' as plain ASCII.
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 2) == uint32_t('A'));
    }

    TEST_CASE("truncated 4-byte sequence (only 3 bytes available)") {
        const char  bytes[] = { (char)0xF0, (char)0x9F, (char)0x98 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 3) == WPTextRenderer::kReplacementChar);
    }
}

TEST_SUITE("WPTextRenderer::DecodeUtf8 — stream behavior") {
    TEST_CASE("end-of-buffer returns 0 (terminator), not FFFD") {
        const char* p = "";
        CHECK(WPTextRenderer::DecodeUtf8(p, p) == 0u);
    }

    TEST_CASE("mixed valid+invalid stream decodes each correctly") {
        // "a" + overlong-null + "b" + lone-surrogate + "c"
        std::string s;
        s.push_back('a');
        s.push_back((char)0xC0);
        s.push_back((char)0x80);
        s.push_back('b');
        s.push_back((char)0xED);
        s.push_back((char)0xA0);
        s.push_back((char)0x80);
        s.push_back('c');
        auto cps = decode_all(s.data(), s.size());
        REQUIRE(cps.size() == 5);
        CHECK(cps[0] == uint32_t('a'));
        CHECK(cps[1] == WPTextRenderer::kReplacementChar);
        CHECK(cps[2] == uint32_t('b'));
        CHECK(cps[3] == WPTextRenderer::kReplacementChar);
        CHECK(cps[4] == uint32_t('c'));
    }
}

#include <atomic>
#include <thread>

// FT_Library is a process-global FreeType resource. Parser thread runs
// Init/Shutdown around each Parse(); render thread calls RenderText
// per text-layer-update. Without serialisation, a parser-thread Shutdown
// can free the library while the render thread holds a live FT_Face
// referencing it. The mutex guard inside WPTextRenderer must serialise
// both. Empty-text exercise is enough to keep the lazy-init re-check
// honest; full raster would also work but isn't needed to prove the race.
TEST_SUITE("WPTextRenderer Init/Shutdown race") {
    TEST_CASE("parallel Init/Shutdown + RenderText does not crash") {
        std::atomic<bool> stop { false };
        std::atomic<int>  render_count { 0 };
        std::thread       render([&] {
            while (! stop.load(std::memory_order_relaxed)) {
                auto img =
                    WPTextRenderer::RenderText("", 16.f, "", 32, 32, "center", "center", 0);
                (void)img;
                render_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
        for (int i = 0; i < 64; ++i) {
            WPTextRenderer::Init();
            WPTextRenderer::Shutdown();
        }
        stop.store(true, std::memory_order_relaxed);
        render.join();
        CHECK(render_count.load() > 0);
    }
}

TEST_SUITE("WPTextRenderer::RenderText — guard clauses") {
    TEST_CASE("empty fontData returns nullptr (logged, not crashed)") {
        // Scripts that lose their font ref must not crash the renderer.
        auto img = WPTextRenderer::RenderText("", 16.f, "X", 32, 32, "center", "center", 0);
        CHECK(img == nullptr);
    }

    TEST_CASE("zero or negative dimensions return nullptr") {
        // FontData with garbage — first dimension guard fires before we
        // ever reach FT_New_Memory_Face, so garbage is fine here.
        std::string fake_font(256, '\0');
        CHECK(WPTextRenderer::RenderText(fake_font, 16.f, "X", 0, 32, "center", "center", 0)
              == nullptr);
        CHECK(WPTextRenderer::RenderText(fake_font, 16.f, "X", 32, 0, "center", "center", 0)
              == nullptr);
        CHECK(WPTextRenderer::RenderText(fake_font, 16.f, "X", -1, 32, "center", "center", 0)
              == nullptr);
        CHECK(WPTextRenderer::RenderText(fake_font, 16.f, "X", 32, -10, "center", "center", 0)
              == nullptr);
    }

    TEST_CASE("invalid font bytes return nullptr (FT_New_Memory_Face fails)") {
        // Random garbage in fontData passes the empty-guard but fails FT
        // parsing. The renderer must return nullptr cleanly, not raise.
        std::string garbage(1024, '\xFF');
        auto        img =
            WPTextRenderer::RenderText(garbage, 16.f, "hello", 64, 64, "center", "center", 0);
        CHECK(img == nullptr);
    }
}

TEST_SUITE("WPTextRenderer::RenderText — empty-text fast path") {
    // text=="" returns a transparent canvas of the requested width/height
    // without touching FreeType. Verifies the contract scripts rely on for
    // clearing labels (per WPTextRenderer.cpp:119-145).
    //
    // Production order: fontData-empty -> nullptr; (w|h)<=0 -> nullptr;
    // text-empty -> transparent canvas. So the empty-text path requires
    // non-empty fontData (the bytes are never parsed because we return
    // before FT_New_Memory_Face). A single placeholder byte is enough.
    TEST_CASE("empty text returns transparent canvas of requested size") {
        const std::string nonEmptyFontPlaceholder = "x"; // never parsed
        auto              img                     = WPTextRenderer::RenderText(
            nonEmptyFontPlaceholder, 16.f, "", 64, 32, "center", "center", 0);
        REQUIRE(img != nullptr);
        CHECK(img->header.width  == 64);
        CHECK(img->header.height == 32);
        CHECK(img->header.mapWidth  == 64);
        CHECK(img->header.mapHeight == 32);
        CHECK(img->header.count == 1);
        CHECK(img->header.format == TextureFormat::RGBA8);
        REQUIRE(img->slots.size() == 1);
        REQUIRE(img->slots[0].mipmaps.size() == 1);
        CHECK(img->slots[0].mipmaps[0].width  == 64);
        CHECK(img->slots[0].mipmaps[0].height == 32);
        CHECK(img->slots[0].mipmaps[0].size   == 64 * 32 * 4);
        // Buffer is zero-initialised → alpha == 0 everywhere → fully
        // transparent. Sample a few bytes to confirm.
        const uint8_t* data = img->slots[0].mipmaps[0].data.get();
        REQUIRE(data != nullptr);
        CHECK(data[0]            == 0);   // R first pixel
        CHECK(data[3]            == 0);   // A first pixel
        CHECK(data[64 * 32 * 4 - 1] == 0);   // A last pixel
    }

    TEST_CASE("empty text honours requested dimensions exactly (1x1)") {
        // Layout helpers depend on dimensions being preserved through the
        // empty-text path (no padding compensation, no minimum sizes).
        const std::string nonEmptyFontPlaceholder = "x";
        auto              img                     = WPTextRenderer::RenderText(
            nonEmptyFontPlaceholder, 16.f, "", 1, 1, "top", "top", 0);
        REQUIRE(img != nullptr);
        CHECK(img->header.width  == 1);
        CHECK(img->header.height == 1);
    }
}

TEST_SUITE("WPTextRenderer::kRasterDpiScale") {
    TEST_CASE("constant is 2.0f (super-sampling factor synced with ParseTextObj)") {
        // Two callers depend on this constant being identical: WPTextRenderer
        // scales pointsize by kRasterDpiScale * (96/72), and ParseTextObj
        // multiplies the authored canvas by kRasterDpiScale so glyphs don't
        // clip. Changing one without the other silently misaligns text.
        CHECK(WPTextRenderer::kRasterDpiScale == 2.0f);
    }
}

// ---------- bitmap ownership ----------
//
// A canary counter for `operator new[]` calls.  std::vector's allocator uses
// `operator new` (single-object form), NOT `operator new[]`, so a `new T[]`
// expression is uniquely identifiable.  The pre-fix RenderText pattern
// allocated a fresh `uint8_t[bufSize]` and full-bitmap memcpy'd into it just
// so ImageDataPtr could carry a `delete[]`-deleter — the redundant copy.
// Post-fix, the buffer is owned by a `shared_ptr<vector<uint8_t>>` captured
// in the type-erased deleter; `operator new[]` is no longer called from
// RenderText.
//
// The override forwards to the standard allocator, so it is safe to leave
// in place for the whole test binary — only the counter is observable.
namespace wek_q5_alloc_canary
{
inline std::atomic<int>& counter() {
    static std::atomic<int> n { 0 };
    return n;
}
} // namespace wek_q5_alloc_canary

// TSan's runtime defines its own `operator new[]/delete[]` to interpose every
// allocation, which collides with our canary overrides at link time.  Skip
// the canary in TSan builds; the underlying invariant (no `new T[]` in
// RenderText) is purely an allocation-pattern check that TSan does not
// stress, and a TSan-clean run remains observable via other tests.
#if ! defined(__has_feature) || ! __has_feature(thread_sanitizer)
void* operator new[](std::size_t n) {
    wek_q5_alloc_canary::counter().fetch_add(1, std::memory_order_relaxed);
    return ::operator new(n);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#endif

TEST_SUITE("WPTextRenderer bitmap ownership") {
    TEST_CASE("RenderText does not allocate a redundant uint8_t[] buffer") {
        // Pin the no-redundant-copy invariant: the rasterized bitmap is owned
        // by a vector (via a shared_ptr<vector> captured in the ImageDataPtr
        // deleter); RenderText must NOT call `new uint8_t[bufSize]` to
        // duplicate the buffer.  vector's allocator goes through
        // `operator new`, NOT `operator new[]`, so the canary catches only
        // the `new T[]` expression that the pre-fix code used.
        //
        // Exercise the empty-text fast path — non-empty fontData + empty text
        // returns a transparent canvas without touching FreeType.  Same
        // ownership pattern as the main path (both sites used
        // `new uint8_t[bufSize]` + delete[]-deleter pre-fix).
        const std::string fontPlaceholder = "x"; // never parsed
        const int         before          = wek_q5_alloc_canary::counter().load();
        auto              img =
            WPTextRenderer::RenderText(fontPlaceholder, 16.f, "", 64, 32, "center", "center", 0);
        const int delta = wek_q5_alloc_canary::counter().load() - before;
        REQUIRE(img != nullptr);
        // Pre-fix: 1 (the `new uint8_t[bufSize]()` at WPTextRenderer.cpp:138).
        // Post-fix: 0 (buffer owned by make_shared<vector>).
        CHECK(delta == 0);
    }

    TEST_CASE("rendered image data outlives the RenderText scope") {
        // Sanity: the ImageDataPtr must keep the bytes alive after RenderText
        // returns — the consumer (TextureCache::ReuploadTex) reads from
        // data.get() after RenderText has unwound.  The pre-fix delete[]
        // deleter satisfied this; the post-fix shared_ptr<vector> capture
        // satisfies it too (deleter holds the shared_ptr by value).
        const std::string fontPlaceholder = "x";
        auto              img =
            WPTextRenderer::RenderText(fontPlaceholder, 16.f, "", 64, 32, "center", "center", 0);
        REQUIRE(img != nullptr);
        REQUIRE(img->slots.size() == 1);
        REQUIRE(img->slots[0].mipmaps.size() == 1);
        const uint8_t* data = img->slots[0].mipmaps[0].data.get();
        REQUIRE(data != nullptr);
        // Read after RenderText returned.  Bytes are zero (empty-text → fully
        // transparent canvas).  Pre-fix: delete[]-owned.  Post-fix:
        // shared_ptr<vector>-owned.  Both valid.
        CHECK(data[0] == 0);
        CHECK(data[64 * 32 * 4 - 1] == 0);
    }

    TEST_CASE("releasing ImageDataPtr frees the underlying bytes (no leak)") {
        // Build many images and drop them.  doctest itself can't observe
        // heap state, but preflight's --sanitize=address leg catches a leak
        // deterministically.  A bug where the deleter failed to drop its
        // captured shared_ptr (e.g., capture-by-reference of a local) would
        // surface as a 1000-vector LeakSanitizer report.
        const std::string fontPlaceholder = "x";
        for (int i = 0; i < 1000; ++i) {
            auto img = WPTextRenderer::RenderText(
                fontPlaceholder, 16.f, "", 128, 128, "center", "center", 0);
            REQUIRE(img != nullptr);
            // img drops here → ImageDataPtr drops → deleter fires →
            // shared_ptr<vector> refcount hits zero → vector dtor frees.
        }
    }
}

// ---------- FT_Face LRU cache ----------
//
// `RenderText` previously parsed the font buffer on every call via
// `FT_New_Memory_Face` + `FT_Done_Face`.  Clock-style wallpapers (e.g. 2866203962
// VHS Time/Date) drive ~9 RenderText calls/sec — each re-parsing the same
// ~200KB-1MB OTF/TTF.  The cache holds at most kMaxFaceCacheEntries live
// faces keyed on the buffer pointer+size, evicted LRU.
//
// These tests exercise the cache via three test-only accessors on
// WPTextRenderer (TEST_getFaceCacheSize / TEST_getLastPixelSize /
// TEST_getFaceCacheCapacity).  Each accessor locks s_ftLibMutex.

TEST_SUITE("WPTextRenderer FT_Face cache") {
    // Liberation Sans Regular is shipped by every preflight host (the
    // liberation-fonts RPM is a Fedora default; Debian/Ubuntu ship it via
    // fonts-liberation).  If absent, skip gracefully — same pattern as
    // the existing test_VideoTextureDecoder env-skip path.
    static std::string loadHostFont() {
        const std::string path = wallpaper::ResolveSystemFontFallback("systemfont_sans");
        if (path.empty()) return {};
        return wallpaper::ReadSystemFile(path);
    }

    TEST_CASE("repeated RenderText with same fontData reuses FT_Face") {
        auto fontData = loadHostFont();
        if (fontData.empty()) {
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        WPTextRenderer::Init();
        for (int i = 0; i < 100; ++i) {
            auto img =
                WPTextRenderer::RenderText(fontData, 16.f, "tick", 64, 32, "center", "center", 0);
            REQUIRE(img != nullptr);
        }
        // After 100 calls with the same buffer the cache holds exactly ONE
        // entry — proves we are NOT re-parsing the font per call.
        CHECK(WPTextRenderer::TEST_getFaceCacheSize() == 1);
        WPTextRenderer::Shutdown();
        // Shutdown must purge the cache before FT_Done_FreeType.
        CHECK(WPTextRenderer::TEST_getFaceCacheSize() == 0);
    }

    TEST_CASE("different fontData buffers cause distinct cache entries") {
        auto fontA = loadHostFont();
        if (fontA.empty()) {
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        std::string fontB = fontA; // independent std::string -> different .data()
        REQUIRE(fontA.data() != fontB.data());
        WPTextRenderer::Init();
        (void)WPTextRenderer::RenderText(fontA, 16.f, "a", 32, 32, "center", "center", 0);
        (void)WPTextRenderer::RenderText(fontB, 16.f, "b", 32, 32, "center", "center", 0);
        CHECK(WPTextRenderer::TEST_getFaceCacheSize() == 2);
        WPTextRenderer::Shutdown();
    }

    TEST_CASE("FT_Set_Pixel_Sizes is tracked per-entry and updates on size change") {
        auto fontData = loadHostFont();
        if (fontData.empty()) {
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        WPTextRenderer::Init();
        (void)WPTextRenderer::RenderText(fontData, 16.f, "a", 32, 32, "center", "center", 0);
        // Compute via the same formula production code uses; assert the
        // accessor matches.  pixelSize = round(points * 96/72 * scale + 0.5).
        unsigned int expected16 = static_cast<unsigned int>(
            16.0f * 96.0f / 72.0f * WPTextRenderer::kRasterDpiScale + 0.5f);
        if (expected16 < 4) expected16 = 4;
        CHECK(WPTextRenderer::TEST_getLastPixelSize(fontData.data()) == expected16);

        (void)WPTextRenderer::RenderText(fontData, 32.f, "a", 32, 32, "center", "center", 0);
        unsigned int expected32 = static_cast<unsigned int>(
            32.0f * 96.0f / 72.0f * WPTextRenderer::kRasterDpiScale + 0.5f);
        CHECK(WPTextRenderer::TEST_getLastPixelSize(fontData.data()) == expected32);
        WPTextRenderer::Shutdown();
    }

    TEST_CASE("LRU evicts oldest entry when at capacity") {
        auto fontData = loadHostFont();
        if (fontData.empty()) {
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        const std::size_t cap = WPTextRenderer::TEST_getFaceCacheCapacity();
        REQUIRE(cap > 0);
        // Synthesise cap+1 distinct std::string buffers — each an independent
        // copy of fontData with a unique .data().  Liberation Sans is many KB
        // so SBO can't collapse pointers; defend with a REQUIRE anyway.
        std::vector<std::string> buffers;
        buffers.reserve(cap + 1);
        for (std::size_t i = 0; i < cap + 1; ++i) buffers.push_back(fontData);
        for (std::size_t i = 1; i < buffers.size(); ++i) {
            REQUIRE(buffers[i].data() != buffers[0].data());
        }
        WPTextRenderer::Init();
        for (auto& b : buffers) {
            (void)WPTextRenderer::RenderText(b, 16.f, "x", 32, 32, "center", "center", 0);
        }
        CHECK(WPTextRenderer::TEST_getFaceCacheSize() == cap);
        // buffers[0] was inserted first and is now evicted at the cap boundary.
        // Re-rendering it is a miss → count stays at cap (which means a now-older
        // entry — buffers[1] — was evicted in turn).
        (void)WPTextRenderer::RenderText(buffers[0], 16.f, "x", 32, 32, "center", "center", 0);
        CHECK(WPTextRenderer::TEST_getFaceCacheSize() == cap);
        WPTextRenderer::Shutdown();
    }

    TEST_CASE("Init -> render -> Shutdown -> Init -> render survives cycle (no UAF)") {
        // Shutdown must purge the cache BEFORE FT_Done_FreeType, otherwise a
        // cached FT_Face dangles on the freed library.  Single-thread
        // companion to the existing Q1 race test.
        auto fontData = loadHostFont();
        if (fontData.empty()) {
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        WPTextRenderer::Init();
        {
            auto img =
                WPTextRenderer::RenderText(fontData, 16.f, "a", 32, 32, "center", "center", 0);
            REQUIRE(img != nullptr);
        }
        WPTextRenderer::Shutdown();
        CHECK(WPTextRenderer::TEST_getFaceCacheSize() == 0);
        WPTextRenderer::Init();
        {
            auto img =
                WPTextRenderer::RenderText(fontData, 16.f, "b", 32, 32, "center", "center", 0);
            REQUIRE(img != nullptr);
        }
        CHECK(WPTextRenderer::TEST_getFaceCacheSize() == 1);
        WPTextRenderer::Shutdown();
    }
}

// ---------- kerning + fallback face ----------
//
// Closes two FreeType-direct call-site bugs in RenderText / MeasureLineWidth:
// (a) FT_HAS_KERNING / FT_Get_Kerning were never invoked, so pairs like AV,
//     To, Te, Wa rendered unkerned even on fonts shipping a `kern` table.
// (b) FT_Load_Char silently loaded the .notdef glyph on a missing codepoint;
//     a CJK string in a Latin-only font rendered as silent boxes with no log.
//     RenderText now counts missing glyphs via FT_Get_Char_Index, emits a
//     rate-limited LOG_INFO per scene, and by default loads a Noto Sans CJK
//     fallback face for codepoints in the Han block; WEKDE_TEXT_CJK_FALLBACK=0
//     disables (opt-out preserved for the .notdef-rendering testing case).

#include <cstdlib> // setenv / unsetenv

TEST_SUITE("WPTextRenderer kerning + fallback") {
    static std::string loadHostFont() {
        const std::string path = wallpaper::ResolveSystemFontFallback("systemfont_sans");
        if (path.empty()) return {};
        return wallpaper::ReadSystemFile(path);
    }

    TEST_CASE("FT_Get_Kerning is invoked between consecutive glyphs on a kerned font") {
        // (a) Direct instrumentation: count the number of FT_Get_Kerning
        // attempts during a rasterization of a string with multiple pairs.
        // The counter is a test-only accessor exposed by WPTextRenderer.
        auto fontData = loadHostFont();
        if (fontData.empty()) {
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        WPTextRenderer::TEST_resetKerningProbeCounter();
        // Kerning is only attempted when FT_HAS_KERNING(face) returns true;
        // record the host's verdict so the assertion can decline gracefully
        // on fonts without a legacy `kern` table.
        auto img =
            WPTextRenderer::RenderText(fontData, 24.f, "AVAVAV", 256, 64, "left", "top", 0);
        REQUIRE(img != nullptr);
        const int attempts = WPTextRenderer::TEST_getKerningProbeCount();
        if (! WPTextRenderer::TEST_hostFontHasKerning(fontData)) {
            MESSAGE("Host's Liberation Sans lacks a legacy `kern` table; kerning "
                    "queries skipped per FreeType semantics (GPOS is HarfBuzz scope).");
            CHECK(attempts == 0);
        } else {
            // 6 glyphs in "AVAVAV" → 5 consecutive pairs that should be
            // probed.  MeasureLineWidth also probes the same 5 pairs, so
            // the count is 10.  Defend with a loose lower bound: probe
            // count must be >= 5 (the raster pass alone produces this).
            CHECK(attempts >= 5);
        }
    }

    TEST_CASE("primary face missing glyph: fallback consulted by default (env unset)") {
        // (b) Default behaviour: when FT_Get_Char_Index on the primary face
        // returns 0, RenderText loads the Noto Sans CJK fallback face (held
        // under s_ftLibMutex, cached via Q2's LRU) and uses its glyph
        // instead of .notdef.  No env var required — that's the opt-OUT
        // direction.
        ::unsetenv("WEKDE_TEXT_CJK_FALLBACK");
        auto fontData = loadHostFont();
        if (fontData.empty()) {
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        if (wallpaper::ResolveCJKHanFallback().empty()) {
            MESSAGE("Noto Sans CJK absent on host; skipping fallback test");
            return;
        }
        WPTextRenderer::TEST_resetFallbackProbeCounter();
        auto img = WPTextRenderer::RenderText(fontData, 24.f, "\xE4\xB8\xAD", 64, 64, "center",
                                              "center", 0);
        const int consulted = WPTextRenderer::TEST_getFallbackProbeCount();
        REQUIRE(img != nullptr);
        // The fallback path must have been consulted at least once for the
        // single Han codepoint U+4E2D.
        CHECK(consulted >= 1);
        // The fallback Han glyph rasterizes through the primary face's
        // ascender/descender baseline.  Sum total alpha across the canvas —
        // a .notdef box has sparse alpha; the actual 中 from Noto is dense.
        // Threshold conservatively (well above any plausible .notdef-only
        // hollow rectangle).
        const uint8_t* data       = img->slots[0].mipmaps[0].data.get();
        int            totalAlpha = 0;
        for (int i = 0; i < 64 * 64; ++i) totalAlpha += data[i * 4 + 3];
        CHECK(totalAlpha > 5000);
    }

    TEST_CASE("primary face missing glyph: WEKDE_TEXT_CJK_FALLBACK=0 disables → replacement glyph + LOG_INFO once") {
        // Mirror of the default-ON case in reverse: with the env var set
        // to "0", RenderText must:
        //   (1) NOT consult the fallback face at all;
        //   (2) still rasterize the .notdef glyph (replacement);
        //   (3) emit LOG_INFO at least once across multiple consecutive
        //       missing-glyph calls (rate-limited to ~once per 32).
        ::setenv("WEKDE_TEXT_CJK_FALLBACK", "0", 1);
        auto fontData = loadHostFont();
        if (fontData.empty()) {
            ::unsetenv("WEKDE_TEXT_CJK_FALLBACK");
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        WPTextRenderer::TEST_resetFallbackProbeCounter();
        WPTextRenderer::TEST_resetMissingGlyphLogCounter();
        std::string cjk = "\xE4\xB8\xAD"; // U+4E2D '中'
        for (int i = 0; i < 100; ++i) {
            (void)WPTextRenderer::RenderText(fontData, 16.f, cjk, 32, 32, "center", "center", 0);
        }
        ::unsetenv("WEKDE_TEXT_CJK_FALLBACK");
        // (1) Fallback must NOT be touched when the env var disables, even
        // if Noto CJK happens to be installed on the host.
        CHECK(WPTextRenderer::TEST_getFallbackProbeCount() == 0);
        // (3) LOG_INFO fires at least once across the 100 calls.
        const int logged = WPTextRenderer::TEST_getMissingGlyphLogCount();
        CHECK(logged >= 1);
        CHECK(logged <= 6);
    }

    TEST_CASE("all-Latin font + Latin string: no fallback-face load") {
        // (c) Gate check: when no codepoint in the string is missing from
        // the primary face, the fallback resolver must not be touched —
        // not even with the default-ON behaviour active.  Defends against
        // an over-eager implementation that loads Noto CJK on every
        // RenderText call.
        ::unsetenv("WEKDE_TEXT_CJK_FALLBACK");
        auto fontData = loadHostFont();
        if (fontData.empty()) {
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        WPTextRenderer::TEST_resetFallbackProbeCounter();
        auto img =
            WPTextRenderer::RenderText(fontData, 24.f, "Hello world", 256, 64, "center", "center", 0);
        REQUIRE(img != nullptr);
        // No missing glyph in the primary → no fallback probe.
        CHECK(WPTextRenderer::TEST_getFallbackProbeCount() == 0);
    }

    TEST_CASE("kerned line width <= unkerned line width") {
        // Sanity for the measure loop: applying kerning never makes the
        // line wider.  Pre-Q4 there was no kerned measure at all; with
        // Q4 the kerned measure must match what the raster loop produces
        // (or right alignment drifts).
        auto fontData = loadHostFont();
        if (fontData.empty()) return;
        const int kerned = WPTextRenderer::TEST_measureLineWidthWithKerning(fontData, 48.f, "AV");
        const int unkern = WPTextRenderer::TEST_measureLineWidthNoKerning(fontData, 48.f, "AV");
        REQUIRE(kerned > 0);
        REQUIRE(unkern > 0);
        CHECK(kerned <= unkern);
    }
}

// -------- FT_Load_Glyph failure logging --------
//
// Splits the previously-conflated "missing codepoint" (idx == 0, benign,
// LOG_INFO) and "FT_Load_Glyph fail" (pathological — corrupt glyf, OOM,
// unsupported variable-axis combo, LOG_ERROR) error classes.  Two
// independent rate-limited counters; this suite verifies the split is
// honest by exercising both paths and asserting the correct counter
// fires (and the other stays at zero).

TEST_SUITE("WPTextRenderer FT_Load_Glyph failure logging") {
    static std::string loadHostFont() {
        const std::string path = wallpaper::ResolveSystemFontFallback("systemfont_sans");
        if (path.empty()) return {};
        return wallpaper::ReadSystemFile(path);
    }

    // Mutate a copy of `fontBytes` (expected: Liberation Sans or another
    // standard TTF) so that some real glyphs in the file fail
    // FT_Load_Glyph while the cmap and table directory still parse.
    //
    // Strategy: walk the `loca` table (long form: uint32 entries) to find
    // each real glyph's byte range inside the `glyf` table, then overwrite
    // a swathe of those glyph records with bytes that produce an invalid
    // composite-glyph descriptor.  Glyph format:
    //   int16 numberOfContours
    //   int16 xMin/yMin/xMax/yMax
    //   [contours / components ...]
    // numberOfContours < 0 marks a composite glyph; subsequent bytes are
    // a chain of (uint16 flags, uint16 glyphIndex, [args...]) entries
    // continued until a flags entry without the MORE_COMPONENTS bit (0x20).
    // We write numberOfContours = -1 (composite) and follow with a flags
    // byte 0xFF (every flag bit set, including a bogus glyph index range
    // and unsupported argument formats) so FreeType's composite walker
    // hits an invalid component before completion and returns an error
    // such as FT_Err_Invalid_Composite.  This is more reliable than
    // poisoning `loca` directly: FT computes glyph length from adjacent
    // loca entries, and matching 0xFFFFFFFE pairs yield length 0 which FT
    // treats as a blank glyph (no error).
    //
    // Returns empty string when mutation isn't possible (tables missing,
    // file too small, short loca format).
    static std::string mutateLiberationSansGlyfTable(std::string fontBytes) {
        auto rd16 = [](const char* p) -> uint16_t {
            auto b = reinterpret_cast<const uint8_t*>(p);
            return uint16_t((uint16_t(b[0]) << 8) | uint16_t(b[1]));
        };
        auto rd32 = [](const char* p) -> uint32_t {
            auto b = reinterpret_cast<const uint8_t*>(p);
            return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) |
                   (uint32_t(b[2]) << 8) | uint32_t(b[3]);
        };
        if (fontBytes.size() < 12) return {};
        uint32_t numTables = rd16(fontBytes.data() + 4);
        if (fontBytes.size() < 12 + size_t(numTables) * 16) return {};
        uint32_t headOffset = 0, headLen = 0;
        uint32_t locaOffset = 0, locaLen = 0;
        uint32_t glyfOffset = 0, glyfLen = 0;
        for (uint32_t i = 0; i < numTables; ++i) {
            const char* entry = fontBytes.data() + 12 + i * 16;
            uint32_t    off   = rd32(entry + 8);
            uint32_t    len   = rd32(entry + 12);
            if (std::memcmp(entry, "head", 4) == 0) {
                headOffset = off;
                headLen    = len;
            } else if (std::memcmp(entry, "loca", 4) == 0) {
                locaOffset = off;
                locaLen    = len;
            } else if (std::memcmp(entry, "glyf", 4) == 0) {
                glyfOffset = off;
                glyfLen    = len;
            }
        }
        if (headOffset == 0 || locaOffset == 0 || glyfOffset == 0) return {};
        // `head.indexToLocFormat` sits at byte 50 of the head table.
        // 0 = short (uint16 entries scaled by 2), 1 = long (uint32 entries).
        if (size_t(headOffset) + 52 > fontBytes.size()) return {};
        if (headLen < 52) return {};
        uint16_t locFormat = rd16(fontBytes.data() + headOffset + 50);
        if (locFormat != 1) return {};
        if (locaLen < 8) return {};
        const uint32_t entryCount = locaLen / 4u;
        // Walk loca[1..N], corrupt each real (non-empty) glyph record.
        // Skip glyph 0 (.notdef) so the missing-codepoint fallback still
        // gets a valid replacement glyph.
        const uint32_t startIdx = 1u;
        const uint32_t endIdx   = std::min(entryCount - 1, 256u);
        if (endIdx <= startIdx) return {};
        int corruptedCount = 0;
        for (uint32_t i = startIdx; i < endIdx; ++i) {
            uint32_t off0 = rd32(fontBytes.data() + locaOffset + i * 4);
            uint32_t off1 = rd32(fontBytes.data() + locaOffset + (i + 1) * 4);
            if (off1 <= off0) continue; // empty glyph
            if (size_t(glyfOffset) + off1 > fontBytes.size()) continue;
            if (off1 - off0 < 12) continue; // too short to be a real glyph
            char* gp = fontBytes.data() + glyfOffset + off0;
            // numberOfContours = -1 (int16, big-endian) → composite glyph
            gp[0] = char(0xFF);
            gp[1] = char(0xFF);
            // xMin/yMin/xMax/yMax (8 bytes) — leave bounding box garbage
            // The composite walker reads (flags, glyphIndex) pairs from
            // offset 10; we set flags = 0xFFFF (every bit set, includes
            // ARG_1_AND_2_ARE_WORDS, WE_HAVE_A_SCALE, MORE_COMPONENTS,
            // WE_HAVE_INSTRUCTIONS, ...) and glyphIndex = 0xFFFF
            // (out-of-range glyph reference).  FreeType's composite
            // resolver must reject these.
            gp[10] = char(0xFF);
            gp[11] = char(0xFF);
            gp[12] = char(0xFF);
            gp[13] = char(0xFF);
            ++corruptedCount;
        }
        if (corruptedCount == 0) return {};
        return fontBytes;
    }

    TEST_CASE("FT_Load_Glyph failure: rate-limited LOG_ERROR fires at multiples of 32") {
        auto fontData = loadHostFont();
        if (fontData.empty()) {
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        auto corrupted = mutateLiberationSansGlyfTable(fontData);
        if (corrupted.empty() || corrupted == fontData) {
            MESSAGE("glyf-table mutator could not corrupt this Liberation Sans; skipping");
            return;
        }
        WPTextRenderer::TEST_resetLoadGlyphFailLogCounter();
        // 100 render calls with the corrupt font + a Latin string wide
        // enough to land at least one glyph in the corrupted window.
        // Some glyphs in "Hello world" may avoid the corruption — that's
        // fine, we only need >= 1 failure per call to drive the counter.
        for (int i = 0; i < 100; ++i) {
            auto img = WPTextRenderer::RenderText(
                corrupted, 16.f, "Hello world", 256, 64, "center", "center", 0);
            (void)img; // may be partial; assertion is on the log counter
        }
        const int logged = WPTextRenderer::TEST_getLoadGlyphFailLogCount();
        // Rate-limit: every 32nd CALL that has any glyph failure logs
        // once.  100 calls all with failures → 4 logs (ticks 0, 32, 64,
        // 96).  Loose bounds defend against ticks already advanced by
        // an earlier test in the same process.
        CHECK(logged >= 1);
        CHECK(logged <= 6);
    }

    TEST_CASE("benign miss (idx == 0, CJK in Latin font): does NOT fire LOG_ERROR counter") {
        // The whole point of splitting the error class: missing
        // codepoint (idx==0) routes to the EXISTING missing-glyph
        // LOG_INFO path; the NEW LOG_ERROR counter must stay at 0.
        ::setenv("WEKDE_TEXT_CJK_FALLBACK", "0", 1); // force .notdef path
        auto fontData = loadHostFont();
        if (fontData.empty()) {
            ::unsetenv("WEKDE_TEXT_CJK_FALLBACK");
            MESSAGE("Liberation Sans not present on host; skipping");
            return;
        }
        WPTextRenderer::TEST_resetMissingGlyphLogCounter();
        WPTextRenderer::TEST_resetLoadGlyphFailLogCounter();
        std::string cjk = "\xE4\xB8\xAD"; // U+4E2D '中'
        for (int i = 0; i < 100; ++i) {
            (void)WPTextRenderer::RenderText(
                fontData, 16.f, cjk, 32, 32, "center", "center", 0);
        }
        ::unsetenv("WEKDE_TEXT_CJK_FALLBACK");
        // Missing-glyph (benign) counter MUST fire — proves the test
        // setup actually triggered the idx==0 path.
        CHECK(WPTextRenderer::TEST_getMissingGlyphLogCount() >= 1);
        // LOAD-GLYPH-FAIL (pathological) counter MUST stay at zero —
        // the .notdef glyph load succeeds; no FT_Load_Glyph error is
        // signalled.
        CHECK(WPTextRenderer::TEST_getLoadGlyphFailLogCount() == 0);
    }
}
