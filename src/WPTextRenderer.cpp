#include "WPTextRenderer.hpp"
#include "SystemFontFallback.hpp"
#include "Utils/Logging.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace wallpaper
{

// FT_Library is a process-global FreeType resource. Init/Shutdown run on
// the parser thread (per Parse() boundary); RenderText runs on the render
// thread. Without serialisation, Shutdown can free the library while
// RenderText holds a live FT_Face that references it. The mutex covers
// the full RenderText body so an in-use face outlives any concurrent
// Shutdown.
static std::mutex s_ftLibMutex;
static FT_Library s_ftLib = nullptr;

// -------- FT_Face LRU cache --------
//
// Pre-cache, every RenderText call ran FT_New_Memory_Face +
// FT_Set_Pixel_Sizes + FT_Done_Face — re-parsing the entire font file
// on each call.  Clock-style wallpapers (e.g. 2866203962 VHS Time/Date)
// drive ~9 RenderText/sec, each re-parsing the same ~200KB-1MB OTF/TTF.
//
// The cache holds at most kMaxFaceCacheEntries live faces keyed on the
// font-buffer (data ptr + size) — caller pointers are stable for the
// lifetime of TextLayer::fontData (any setTextStyle that rebuilds the
// buffer yields a different .data() pointer and correctly misses).
//
// All cache operations happen under s_ftLibMutex — no separate lock,
// no nested-lock risk.  Each entry owns a std::string COPY of the font
// payload because FT_New_Memory_Face does NOT copy the bytes — it
// expects the caller to keep the buffer alive for the face's lifetime.
// Owning a copy decouples the cache from the scene's tl.fontData
// (which may be freed by setTextStyle while the old face still sits
// in the LRU awaiting eviction).
//
// Shutdown MUST purge cached faces BEFORE FT_Done_FreeType — cached
// FT_Face objects internally reference s_ftLib and would dangle on
// the freed library otherwise.
namespace
{
struct FaceCacheKey {
    const void* dataPtr;
    std::size_t dataSize;
    bool        operator==(const FaceCacheKey& o) const noexcept {
        return dataPtr == o.dataPtr && dataSize == o.dataSize;
    }
};
struct FaceCacheKeyHash {
    std::size_t operator()(const FaceCacheKey& k) const noexcept {
        return std::hash<const void*> {}(k.dataPtr) ^ (std::hash<std::size_t> {}(k.dataSize) << 1);
    }
};
struct FaceCacheEntry {
    FaceCacheKey key;
    std::string  bytes; // owns the font payload (FT does NOT copy)
    FT_Face      face { nullptr };
    FT_UInt      lastPixelSize { 0 };
};
constexpr std::size_t     kMaxFaceCacheEntries = 8;
std::list<FaceCacheEntry> s_faceLru;
std::unordered_map<FaceCacheKey, std::list<FaceCacheEntry>::iterator, FaceCacheKeyHash> s_faceIdx;

// Acquire (or create) an FT_Face for `fontData` and set its pixel size.
// MUST be called under s_ftLibMutex.  Returns nullptr on FT failure.
// The returned face is owned by the cache — caller must NOT FT_Done_Face it.
FT_Face acquireFaceLocked(const std::string& fontData, FT_UInt pixelSize) {
    FaceCacheKey key { fontData.data(), fontData.size() };
    if (auto it = s_faceIdx.find(key); it != s_faceIdx.end()) {
        // Hit — promote to MRU and only re-set pixel size on change.
        s_faceLru.splice(s_faceLru.begin(), s_faceLru, it->second);
        FaceCacheEntry& e = s_faceLru.front();
        if (e.lastPixelSize != pixelSize) {
            FT_Set_Pixel_Sizes(e.face, 0, pixelSize);
            e.lastPixelSize = pixelSize;
        }
        return e.face;
    }
    // Miss — evict at capacity, then create.
    while (s_faceLru.size() >= kMaxFaceCacheEntries) {
        auto& victim = s_faceLru.back();
        if (victim.face) FT_Done_Face(victim.face);
        s_faceIdx.erase(victim.key);
        s_faceLru.pop_back();
    }
    FaceCacheEntry entry;
    entry.key    = key;
    entry.bytes  = fontData; // owned copy — FT_New_Memory_Face does NOT copy
    FT_Error err = FT_New_Memory_Face(s_ftLib,
                                      reinterpret_cast<const FT_Byte*>(entry.bytes.data()),
                                      static_cast<FT_Long>(entry.bytes.size()),
                                      0,
                                      &entry.face);
    if (err || ! entry.face) {
        LOG_ERROR("acquireFaceLocked: FT_New_Memory_Face failed: %d", err);
        return nullptr;
    }
    FT_Set_Pixel_Sizes(entry.face, 0, pixelSize);
    entry.lastPixelSize = pixelSize;
    s_faceLru.push_front(std::move(entry));
    s_faceIdx[key] = s_faceLru.begin();
    return s_faceLru.front().face;
}

// Free all cached FT_Face objects.  MUST be called under s_ftLibMutex.
// Called by Shutdown BEFORE FT_Done_FreeType (cached faces internally
// reference s_ftLib; freeing the library invalidates them).
void purgeFaceCacheLocked() {
    for (auto& e : s_faceLru) {
        if (e.face) FT_Done_Face(e.face);
    }
    s_faceLru.clear();
    s_faceIdx.clear();
}

// -------- kerning + missing-glyph + CJK fallback --------
//
// Closes two FreeType-direct gaps:
//   (a) FT_HAS_KERNING / FT_Get_Kerning were never invoked.  Pairs like AV,
//       To, Te, Wa rendered unkerned even on fonts that ship a `kern` table.
//   (b) FT_Load_Char silently loaded .notdef on a missing codepoint; mixed-
//       script text (CJK in a Latin font) rendered as silent boxes with no
//       log line for the user to investigate.  Switch to explicit
//       FT_Get_Char_Index → kerning delta → FT_Load_Glyph; count missing
//       glyphs and rate-limit a LOG_INFO.  By default, load a Noto Sans CJK
//       fallback face (held under s_ftLibMutex like the primary cache) and
//       render the Han glyph through it instead of emitting .notdef;
//       WEKDE_TEXT_CJK_FALLBACK=0 disables (opt-out preserved for the
//       .notdef-rendering testing case).
//
// Out of scope (deferred subitem): HarfBuzz-driven combining marks, RTL,
// color emoji, complex-script shaping, GPOS kerning beyond the legacy
// `kern` table.

// Counts FT_Get_Kerning attempts process-wide.  Test-only observable.
std::atomic<int> s_kerningProbeCount { 0 };
// Counts CJK fallback consults (i.e. how many times the missing-glyph branch
// invoked acquireFallbackFaceLocked) process-wide.  Test-only observable.
std::atomic<int> s_fallbackProbeCount { 0 };
// Rate-limit state for the per-call missing-glyph LOG_INFO.  Ticks on every
// call that has any missing glyph; fires LOG_INFO once per 32 ticks.
std::atomic<int> s_missingGlyphLogTick { 0 };
std::atomic<int> s_missingGlyphLogFired { 0 };

// Query the legacy `kern` table for the (prevIdx, idx) pair.  Returns the
// horizontal kerning in pixels (already grid-fitted by FT_KERNING_DEFAULT
// and the >>6 6.6-fixed-point shift).  Caller MUST gate on hasKerning ==
// FT_HAS_KERNING(face) and on both indices being nonzero — calling
// FT_Get_Kerning with idx==0 (missing glyph) is undefined per FT docs.
//
// Counted on every invocation regardless of return value so the test
// instrumentation can prove the kerning probe is reached.  GPOS-table
// kerning is HarfBuzz scope (most fonts shipping `kern` duplicate the
// data in GPOS; the legacy table covers ~90% of Latin typographic pairs).
int getKerningDelta(FT_Face face, FT_UInt prevIdx, FT_UInt idx, bool hasKerning) {
    if (! hasKerning || prevIdx == 0 || idx == 0) return 0;
    s_kerningProbeCount.fetch_add(1, std::memory_order_relaxed);
    FT_Vector kern;
    if (FT_Get_Kerning(face, prevIdx, idx, FT_KERNING_DEFAULT, &kern) != 0) return 0;
    return static_cast<int>(kern.x >> 6);
}

// Han-script codepoint test for the Tier-2 CJK fallback.  Covers CJK
// Unified Ideographs (U+4E00..U+9FFF), the CJK Symbols & Punctuation block
// (U+3000..U+303F, used in Chinese/Japanese punctuation), Hiragana
// (U+3040..U+309F), Katakana (U+30A0..U+30FF), Hangul Syllables
// (U+AC00..U+D7AF), CJK Compatibility Ideographs (U+F900..U+FAFF), and the
// supplementary Han plane (U+20000..U+2FFFF).  Narrow enough to skip
// Cyrillic, Arabic, Devanagari (which would need their own fallback fonts).
bool isHanCodepoint(uint32_t cp) {
    return (cp >= 0x3000u && cp <= 0x9FFFu) || (cp >= 0xAC00u && cp <= 0xD7AFu) ||
           (cp >= 0xF900u && cp <= 0xFAFFu) || (cp >= 0x20000u && cp <= 0x2FFFFu);
}

// Lazily-loaded CJK Han fallback face.  One static face per process; lives
// behind s_ftLibMutex.  The cache holds an owned copy of the font bytes
// (FT_New_Memory_Face does NOT copy).  Cleared by Shutdown.
//
// Distinct lifetime from the primary face cache — the candidate font is
// resolved from the system, not from the caller's tl.fontData, so the
// "key" is just "the one CJK font we picked".  No need to plumb through
// the LRU.
std::string s_fallbackFontBytes;
FT_Face     s_fallbackFace { nullptr };
FT_UInt     s_fallbackPixelSize { 0 };

// MUST be called under s_ftLibMutex.  Loads the fallback face on first
// call (returns nullptr if no CJK font is installed) and sets pixel size.
FT_Face acquireFallbackFaceLocked(FT_UInt pixelSize) {
    s_fallbackProbeCount.fetch_add(1, std::memory_order_relaxed);
    if (! s_fallbackFace) {
        std::string path = ResolveCJKHanFallback();
        if (path.empty()) return nullptr;
        s_fallbackFontBytes = ReadSystemFile(path);
        if (s_fallbackFontBytes.empty()) return nullptr;
        FT_Error err =
            FT_New_Memory_Face(s_ftLib,
                               reinterpret_cast<const FT_Byte*>(s_fallbackFontBytes.data()),
                               static_cast<FT_Long>(s_fallbackFontBytes.size()),
                               0,
                               &s_fallbackFace);
        if (err || ! s_fallbackFace) {
            s_fallbackFace = nullptr;
            s_fallbackFontBytes.clear();
            return nullptr;
        }
        LOG_INFO("WPTextRenderer: loaded CJK Han fallback face from %s", path.c_str());
    }
    if (s_fallbackPixelSize != pixelSize) {
        FT_Set_Pixel_Sizes(s_fallbackFace, 0, pixelSize);
        s_fallbackPixelSize = pixelSize;
    }
    return s_fallbackFace;
}

// MUST be called under s_ftLibMutex.  Frees the fallback face BEFORE
// FT_Done_FreeType so the FT_Face does not dangle on the freed library.
void purgeFallbackFaceLocked() {
    if (s_fallbackFace) {
        FT_Done_Face(s_fallbackFace);
        s_fallbackFace = nullptr;
    }
    s_fallbackFontBytes.clear();
    s_fallbackPixelSize = 0;
}

} // namespace

void WPTextRenderer::Init() {
    std::lock_guard<std::mutex> lock(s_ftLibMutex);
    if (s_ftLib != nullptr) return;
    FT_Error err = FT_Init_FreeType(&s_ftLib);
    if (err) {
        LOG_ERROR("FT_Init_FreeType failed: %d", err);
        s_ftLib = nullptr;
    }
}

void WPTextRenderer::Shutdown() {
    std::lock_guard<std::mutex> lock(s_ftLibMutex);
    // Purge cached faces BEFORE freeing the library; cached FT_Face
    // objects internally reference s_ftLib and would dangle otherwise.
    purgeFaceCacheLocked();
    purgeFallbackFaceLocked();
    if (s_ftLib) {
        FT_Done_FreeType(s_ftLib);
        s_ftLib = nullptr;
    }
}

std::size_t WPTextRenderer::TEST_getFaceCacheSize() {
    std::lock_guard<std::mutex> lock(s_ftLibMutex);
    return s_faceLru.size();
}

unsigned int WPTextRenderer::TEST_getLastPixelSize(const void* fontDataPtr) {
    std::lock_guard<std::mutex> lock(s_ftLibMutex);
    for (const auto& e : s_faceLru) {
        if (e.key.dataPtr == fontDataPtr) {
            return static_cast<unsigned int>(e.lastPixelSize);
        }
    }
    return 0;
}

std::size_t WPTextRenderer::TEST_getFaceCacheCapacity() {
    return kMaxFaceCacheEntries; // compile-time constant; no lock needed
}

// DecodeUtf8 now lives header-inline at WPTextRenderer::DecodeUtf8 so the
// validation can be tested directly without linking the full wpScene library.

// Split text into lines by '\n'
static std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string              cur;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    lines.push_back(cur);
    return lines;
}

// Compute effective advance for a glyph — max of typographic advance and visual right edge.
// Decorative/3D fonts often have bitmaps wider than their advance width.
static int EffectiveAdvance(FT_GlyphSlot g) {
    int adv        = static_cast<int>(g->advance.x >> 6);
    int bearingX   = static_cast<int>(g->metrics.horiBearingX >> 6);
    int glyphWidth = static_cast<int>(g->metrics.width >> 6);
    int visualEnd  = bearingX + glyphWidth;
    return std::max(adv, visualEnd);
}

// Measure the pixel width of a single line — kerned variant.  Must match
// the raster loop's pen advance byte-for-byte (same FT_Get_Char_Index →
// kerning delta → FT_Load_Glyph sequence) or right/center alignment drifts
// by the cumulative kerning.
static int MeasureLineWidth(FT_Face face, const std::string& line) {
    const bool  hasKerning = FT_HAS_KERNING(face);
    int         pen_x      = 0;
    FT_UInt     prevIdx    = 0;
    const char* p          = line.data();
    const char* end        = p + line.size();
    while (p < end) {
        uint32_t cp = WPTextRenderer::DecodeUtf8(p, end);
        if (cp == 0) break;
        FT_UInt idx = FT_Get_Char_Index(face, cp);
        // idx == 0 → .notdef.  For measurement we still advance by the
        // .notdef glyph's metrics — the raster loop also rasterizes it,
        // and the two paths must agree on the width.
        pen_x += getKerningDelta(face, prevIdx, idx, hasKerning);
        if (FT_Load_Glyph(face, idx, FT_LOAD_DEFAULT) != 0) {
            prevIdx = idx;
            continue;
        }
        pen_x += EffectiveAdvance(face->glyph);
        prevIdx = idx;
    }
    return pen_x;
}

// Unkerned MeasureLineWidth — test-only baseline for TEST_measureLineWidthNoKerning.
// Mirrors the pre-Q4 measure loop verbatim.  Do NOT delete: the kerned-vs-unkerned
// comparison test relies on this.
static int MeasureLineWidthUnkerned(FT_Face face, const std::string& line) {
    int         pen_x = 0;
    const char* p     = line.data();
    const char* end   = p + line.size();
    while (p < end) {
        uint32_t cp = WPTextRenderer::DecodeUtf8(p, end);
        if (cp == 0) break;
        if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) != 0) continue;
        pen_x += EffectiveAdvance(face->glyph);
    }
    return pen_x;
}

std::shared_ptr<Image> WPTextRenderer::RenderText(const std::string& fontData, float pointsize,
                                                  const std::string& text, i32 width, i32 height,
                                                  const std::string& halign,
                                                  const std::string& valign, i32 padding) {
    // Hold the FT_Library mutex for the full body: the FT_Face created
    // below references s_ftLib internally, and a concurrent Shutdown that
    // freed the library while we still held the face would invalidate it
    // mid-raster.
    std::lock_guard<std::mutex> ftLock(s_ftLibMutex);
    if (! s_ftLib) {
        // Lazy init — scene parser shuts down after Parse(), but render
        // thread needs FreeType for dynamic text re-rasterization.
        // We're already holding s_ftLibMutex, so call FT_Init_FreeType
        // directly instead of recursing through Init() (which would
        // re-take the same mutex).
        FT_Error err = FT_Init_FreeType(&s_ftLib);
        if (err) {
            LOG_ERROR("WPTextRenderer: FreeType lazy init failed: %d", err);
            s_ftLib = nullptr;
            return nullptr;
        }
    }
    if (fontData.empty()) {
        LOG_ERROR("WPTextRenderer: empty font data");
        return nullptr;
    }
    if (width <= 0 || height <= 0) {
        LOG_ERROR("WPTextRenderer: invalid dimensions %dx%d", width, height);
        return nullptr;
    }

    // Empty text: return a fully-transparent canvas of the requested size.
    // Scripts clearing a label (e.g. wallpaper 2866203962 playervolume
    // cursorUp does `percentageLayer.text = ""`) rely on this to actually
    // blank the texture — without it the old bitmap (e.g. "100%") persists.
    if (text.empty()) {
        usize bufSize    = static_cast<usize>(width) * static_cast<usize>(height) * 4;
        auto  img_ptr    = std::make_shared<Image>();
        auto& img        = *img_ptr;
        img.header.width = img.header.mapWidth = width;
        img.header.height = img.header.mapHeight = height;
        img.header.format                        = TextureFormat::RGBA8;
        img.header.count                         = 1;
        Image::Slot slot;
        slot.width  = width;
        slot.height = height;
        ImageData mipmap;
        mipmap.width  = width;
        mipmap.height = height;
        mipmap.size   = static_cast<isize>(bufSize);
        // Own the buffer via shared_ptr<vector> captured in ImageDataPtr's
        // type-erased deleter — vector zero-initialises to RGBA=(0,0,0,0),
        // matching the previous `new uint8_t[bufSize]()` value-init.
        auto bufHolder = std::make_shared<std::vector<uint8_t>>(bufSize, 0);
        mipmap.data    = ImageDataPtr(bufHolder->data(), [bufHolder](uint8_t*) {
            // Buffer is owned by the captured shared_ptr<vector>;
            // its destructor frees the bytes when the deleter dies.
        });
        slot.mipmaps.push_back(std::move(mipmap));
        img.slots.push_back(std::move(slot));
        return img_ptr;
    }

    // Convert pointsize from typographic points to pixels.
    // WE authors a scene against a 3840x2160 virtual ortho — pointsize is
    // intended to render at the native full-resolution scale, not at 96 DPI
    // of the VIEWER.  At the 2x implied "Retina"-style DPI the glyphs end
    // up the correct relative size for the reference look (comparing against
    // the YouTube capture of wallpaper 2866203962's VHS Time/Date label).
    // Net formula: points * 96/72 * 2 = points * 8/3 ≈ 2.67 × pointsize.
    // ParseTextObj applies the same kRasterDpiScale to the texture canvas
    // so glyphs don't clip at the edges of an authored 1× size.
    i32 pixelSize = static_cast<i32>(pointsize * 96.0f / 72.0f * kRasterDpiScale + 0.5f);
    if (pixelSize < 4) pixelSize = 4;

    // Cached FT_Face acquisition.  Replaces per-call
    // FT_New_Memory_Face + FT_Set_Pixel_Sizes + FT_Done_Face with an LRU
    // lookup under s_ftLibMutex (already held).  The cache owns the face;
    // we do NOT FT_Done_Face it here.
    FT_Face face = acquireFaceLocked(fontData, static_cast<FT_UInt>(pixelSize));
    if (! face) return nullptr;

    i32 usableW = width - padding * 2;
    i32 usableH = height - padding * 2;
    if (usableW < 1) usableW = 1;
    if (usableH < 1) usableH = 1;

    // Allocate RGBA buffer (white text, alpha = coverage).  Own via a
    // shared_ptr<vector> so the final ImageDataPtr can keep the bytes alive
    // without an extra new[]+memcpy.  `buf` stays a reference so the existing
    // raster body (buf[idx + n] = ...) is unchanged.  The vector is never
    // resize()/reserve()-ed after construction; bufHolder->data() is stable
    // for its lifetime.
    usize bufSize   = static_cast<usize>(width) * static_cast<usize>(height) * 4;
    auto  bufHolder = std::make_shared<std::vector<uint8_t>>(bufSize, 0);
    auto& buf       = *bufHolder;

    i32 ascender  = static_cast<i32>(face->size->metrics.ascender >> 6);
    i32 descender = static_cast<i32>(face->size->metrics.descender >> 6); // negative
    i32 lineH     = ascender - descender;

    auto lines  = SplitLines(text);
    i32  totalH = lineH * static_cast<i32>(lines.size());

    // Vertical start position (baseline of first line)
    i32 startY;
    if (valign == "top") {
        startY = padding + ascender;
    } else if (valign == "bottom") {
        startY = height - padding - totalH + ascender;
    } else { // center
        startY = padding + (usableH - totalH) / 2 + ascender;
    }

    // Kerning is queried once per call (cheap — single FT_HAS_KERNING flag
    // bit).  Both MeasureLineWidth and the raster loop honour the same
    // hasKerning verdict so alignment stays in sync.
    const bool hasKerning            = FT_HAS_KERNING(face);
    int        missingGlyphsThisCall = 0;
    // Re-read the env var on every call rather than caching in a static —
    // the test suite toggles WEKDE_TEXT_CJK_FALLBACK between cases, so a
    // process-lifetime cache would lock in the first value seen.  The
    // getenv cost (~30ns) is dwarfed by the per-call FT raster work.
    // Semantics: default ON; WEKDE_TEXT_CJK_FALLBACK=0 disables (opt-out).
    const char* cjkEnv        = std::getenv("WEKDE_TEXT_CJK_FALLBACK");
    const bool  cjkFallbackOn = ! (cjkEnv && cjkEnv[0] == '0');

    for (usize li = 0; li < lines.size(); ++li) {
        const auto& line = lines[li];
        if (line.empty()) continue;

        i32 lineWidth = MeasureLineWidth(face, line);

        // Horizontal offset
        i32 startX;
        if (halign == "left") {
            startX = padding;
        } else if (halign == "right") {
            startX = padding + usableW - lineWidth;
        } else { // center
            startX = padding + (usableW - lineWidth) / 2;
        }

        i32 baselineY = startY + static_cast<i32>(li) * lineH;
        i32 pen_x     = startX;
        // Track previous glyph index for FT_Get_Kerning.  Reset per line —
        // kerning across a newline is not a thing.
        FT_UInt prevIdx = 0;

        const char* p   = line.data();
        const char* end = p + line.size();
        while (p < end) {
            uint32_t cp = WPTextRenderer::DecodeUtf8(p, end);
            if (cp == 0) break;
            // Explicit glyph-index lookup.  FT_Load_Char would silently
            // substitute .notdef on miss; FT_Get_Char_Index returns 0 so
            // we can count the miss and (optionally) route to the
            // fallback face.
            FT_UInt idx       = FT_Get_Char_Index(face, cp);
            FT_Face faceToUse = face;
            if (idx == 0) {
                ++missingGlyphsThisCall;
                // Tier-2 CJK fallback — default ON, opt-OUT via
                // WEKDE_TEXT_CJK_FALLBACK=0.  Only attempted for codepoints
                // in the Han block (CJK Unified Ideographs + Hangul + Kana
                // + CJK punctuation) so Cyrillic / Arabic / Devanagari
                // still fall through to .notdef + log.
                if (cjkFallbackOn && isHanCodepoint(cp)) {
                    FT_Face fb = acquireFallbackFaceLocked(static_cast<FT_UInt>(pixelSize));
                    if (fb) {
                        FT_UInt fbIdx = FT_Get_Char_Index(fb, cp);
                        if (fbIdx != 0) {
                            faceToUse = fb;
                            idx       = fbIdx;
                            // missingGlyphsThisCall stays incremented so
                            // the LOG_INFO still surfaces the font/script
                            // mismatch — the fallback is a render-quality
                            // band-aid, not a "this font covered it" win.
                        }
                    }
                }
            }
            // Kerning is queried against the primary face (the only one
            // with a known kern table).  When idx is a fallback-face glyph
            // the call returns 0 — FT_Get_Kerning won't find the pair —
            // and pen_x is unaffected.  Cross-face kerning is HarfBuzz scope.
            pen_x += getKerningDelta(face, prevIdx, idx, hasKerning);
            if (FT_Load_Glyph(faceToUse, idx, FT_LOAD_RENDER) != 0) {
                prevIdx = idx;
                continue;
            }

            FT_GlyphSlot g  = faceToUse->glyph;
            i32          bx = pen_x + g->bitmap_left;
            i32          by = baselineY - g->bitmap_top;

            for (u32 row = 0; row < g->bitmap.rows; ++row) {
                for (u32 col = 0; col < g->bitmap.width; ++col) {
                    i32 px = bx + static_cast<i32>(col);
                    i32 py = by + static_cast<i32>(row);
                    if (px < 0 || px >= width || py < 0 || py >= height) continue;

                    uint8_t alpha = g->bitmap.buffer[row * g->bitmap.pitch + col];
                    if (alpha == 0) continue;

                    usize bidx = (static_cast<usize>(py) * static_cast<usize>(width) +
                                  static_cast<usize>(px)) *
                                 4;
                    // White text, alpha = glyph coverage (max blend for overlapping glyphs)
                    buf[bidx + 0] = 255;
                    buf[bidx + 1] = 255;
                    buf[bidx + 2] = 255;
                    buf[bidx + 3] = std::max(buf[bidx + 3], alpha);
                }
            }
            pen_x += EffectiveAdvance(g);
            prevIdx = idx;
        }
    }

    // Rate-limited missing-glyph log.  ~once per 32 calls that hit any
    // missing glyph.  Acceptable journal density for a clock wallpaper
    // that ticks through missing CJK glyphs at 1Hz (~one line every 32s).
    if (missingGlyphsThisCall > 0) {
        int n = s_missingGlyphLogTick.fetch_add(1, std::memory_order_relaxed);
        if ((n & 31) == 0) {
            s_missingGlyphLogFired.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO("WPTextRenderer: %d codepoint(s) missing in font (.notdef glyph "
                     "emitted) in text \"%.32s\" — font may not cover the script",
                     missingGlyphsThisCall,
                     text.c_str());
        }
    }

    // NB: no FT_Done_Face — face is owned by the LRU cache; Shutdown purges.

    // Build Image
    auto  img_ptr        = std::make_shared<Image>();
    auto& img            = *img_ptr;
    img.header.width     = width;
    img.header.height    = height;
    img.header.mapWidth  = width;
    img.header.mapHeight = height;
    img.header.format    = TextureFormat::RGBA8;
    img.header.count     = 1;

    Image::Slot slot;
    slot.width  = width;
    slot.height = height;

    ImageData mipmap;
    mipmap.width  = width;
    mipmap.height = height;
    mipmap.size   = static_cast<isize>(bufSize);

    // Hand the existing shared_ptr<vector>'s buffer to ImageDataPtr.  The
    // captured shared_ptr keeps the vector (and the bytes) alive until the
    // consumer releases the ImageDataPtr — typically right after ReuploadTex
    // (TextureCache.cpp) stages the bytes into a Vulkan staging buffer.
    // Replaces a `new uint8_t[bufSize]` + full-bitmap memcpy that the
    // delete[]-deleter pattern previously required.
    mipmap.data = ImageDataPtr(bufHolder->data(), [bufHolder](uint8_t*) {
        // Buffer is owned by the captured shared_ptr<vector>.
    });

    slot.mipmaps.push_back(std::move(mipmap));
    img.slots.push_back(std::move(slot));

    LOG_INFO("WPTextRenderer: rasterized %dx%d, %zu lines, pointsize=%.0f, text=\"%s\"",
             width,
             height,
             lines.size(),
             pointsize,
             text.c_str());

    // Optional debug dump — set WEKDE_TEXT_DUMP_DIR=/tmp/foo to write each
    // rasterized text bitmap to PPM for diagnosis.
    if (const char* dumpDir = std::getenv("WEKDE_TEXT_DUMP_DIR")) {
        static std::atomic<int> s_dumpSeq { 0 };
        int                     n = s_dumpSeq.fetch_add(1) + 1;
        char                    path[512];
        std::snprintf(path,
                      sizeof(path),
                      "%s/text_%04d_%dx%d_p%d.ppm",
                      dumpDir,
                      n,
                      width,
                      height,
                      (int)(pointsize + 0.5f));
        FILE* fp = std::fopen(path, "wb");
        if (fp) {
            std::fprintf(fp, "P6\n%d %d\n255\n", width, height);
            // RGBA → RGB with alpha as visible white-on-black
            std::vector<uint8_t> rgb(static_cast<usize>(width) * static_cast<usize>(height) * 3);
            for (usize i = 0; i < static_cast<usize>(width) * static_cast<usize>(height); ++i) {
                uint8_t a      = buf[i * 4 + 3];
                rgb[i * 3]     = a;
                rgb[i * 3 + 1] = a;
                rgb[i * 3 + 2] = a;
            }
            std::fwrite(rgb.data(), 1, rgb.size(), fp);
            std::fclose(fp);
        }
    }

    return img_ptr;
}

// -------- test-only accessors --------
//
// All four "measure" / "host kerning" probes build a one-shot FT_Face via
// FT_New_Memory_Face → FT_Done_Face rather than going through Q2's LRU.
// Going through the LRU would inflate cache-size in the FT_Face cache test
// suite; the one-shot face cost is acceptable for a test path that runs
// O(10) times per binary.

int WPTextRenderer::TEST_measureLineWidthWithKerning(const std::string& fontData, float pointsize,
                                                     const std::string& line) {
    std::lock_guard<std::mutex> lock(s_ftLibMutex);
    if (! s_ftLib) {
        FT_Error err = FT_Init_FreeType(&s_ftLib);
        if (err) return -1;
    }
    i32 pixelSize = static_cast<i32>(pointsize * 96.0f / 72.0f * kRasterDpiScale + 0.5f);
    if (pixelSize < 4) pixelSize = 4;
    FT_Face  face = nullptr;
    FT_Error err  = FT_New_Memory_Face(s_ftLib,
                                      reinterpret_cast<const FT_Byte*>(fontData.data()),
                                      static_cast<FT_Long>(fontData.size()),
                                      0,
                                      &face);
    if (err || ! face) return -1;
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize));
    int w = MeasureLineWidth(face, line);
    FT_Done_Face(face);
    return w;
}

int WPTextRenderer::TEST_measureLineWidthNoKerning(const std::string& fontData, float pointsize,
                                                   const std::string& line) {
    std::lock_guard<std::mutex> lock(s_ftLibMutex);
    if (! s_ftLib) {
        FT_Error err = FT_Init_FreeType(&s_ftLib);
        if (err) return -1;
    }
    i32 pixelSize = static_cast<i32>(pointsize * 96.0f / 72.0f * kRasterDpiScale + 0.5f);
    if (pixelSize < 4) pixelSize = 4;
    FT_Face  face = nullptr;
    FT_Error err  = FT_New_Memory_Face(s_ftLib,
                                      reinterpret_cast<const FT_Byte*>(fontData.data()),
                                      static_cast<FT_Long>(fontData.size()),
                                      0,
                                      &face);
    if (err || ! face) return -1;
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize));
    int w = MeasureLineWidthUnkerned(face, line);
    FT_Done_Face(face);
    return w;
}

void WPTextRenderer::TEST_resetKerningProbeCounter() {
    s_kerningProbeCount.store(0, std::memory_order_relaxed);
}

int WPTextRenderer::TEST_getKerningProbeCount() {
    return s_kerningProbeCount.load(std::memory_order_relaxed);
}

bool WPTextRenderer::TEST_hostFontHasKerning(const std::string& fontData) {
    std::lock_guard<std::mutex> lock(s_ftLibMutex);
    if (! s_ftLib) {
        FT_Error err = FT_Init_FreeType(&s_ftLib);
        if (err) return false;
    }
    FT_Face  face = nullptr;
    FT_Error err  = FT_New_Memory_Face(s_ftLib,
                                      reinterpret_cast<const FT_Byte*>(fontData.data()),
                                      static_cast<FT_Long>(fontData.size()),
                                      0,
                                      &face);
    if (err || ! face) return false;
    bool hk = FT_HAS_KERNING(face);
    FT_Done_Face(face);
    return hk;
}

void WPTextRenderer::TEST_resetFallbackProbeCounter() {
    s_fallbackProbeCount.store(0, std::memory_order_relaxed);
}

int WPTextRenderer::TEST_getFallbackProbeCount() {
    return s_fallbackProbeCount.load(std::memory_order_relaxed);
}

void WPTextRenderer::TEST_resetMissingGlyphLogCounter() {
    s_missingGlyphLogTick.store(0, std::memory_order_relaxed);
    s_missingGlyphLogFired.store(0, std::memory_order_relaxed);
}

int WPTextRenderer::TEST_getMissingGlyphLogCount() {
    return s_missingGlyphLogFired.load(std::memory_order_relaxed);
}

} // namespace wallpaper
