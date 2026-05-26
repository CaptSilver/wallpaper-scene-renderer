#include <doctest.h>

#include "WPTextRenderer.hpp"

#include <cstring>
#include <string>

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
