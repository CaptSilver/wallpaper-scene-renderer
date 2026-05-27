#pragma once
#include "Image.hpp"
#include "Core/Literals.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace wallpaper
{

class WPTextRenderer {
public:
    // Raster "super-sampling" multiplier.  WE authors against a 3840×2160
    // virtual ortho; the pointsize is scaled by this factor when converting
    // to pixel size so the rasterized glyphs match the reference look, and
    // ParseTextObj scales authored canvas dimensions by the same factor so
    // the glyphs don't clip at the texture edges.  Keep the two in sync.
    static constexpr float kRasterDpiScale = 2.0f;

    // U+FFFD REPLACEMENT CHARACTER — what we emit for any invalid UTF-8.
    static constexpr uint32_t kReplacementChar = 0xFFFDu;

    static void Init();
    static void Shutdown();

    // Decode one UTF-8 codepoint, advance `p`.  Returns 0 at end-of-buffer.
    // Returns `kReplacementChar` (U+FFFD) for any invalid sequence:
    //   - leading byte that isn't a valid 1/2/3/4-byte start
    //   - missing or non-continuation trailing byte
    //   - overlong encoding (e.g., `C0 80` for U+0000)
    //   - UTF-16 surrogate half (U+D800–U+DFFF — not a valid scalar value)
    //   - codepoint above Unicode max (U+10FFFF; 4-byte path can carry up
    //     to U+1FFFFF, which FreeType can crash on for some fonts)
    // Inline + header-only so unit tests can call it without linking the
    // full wpScene library.
    static inline uint32_t DecodeUtf8(const char*& p, const char* end) {
        if (p >= end) return 0;
        auto     b  = static_cast<uint8_t>(*p);
        uint32_t cp = 0;
        int      n  = 0;
        if (b < 0x80) {
            cp = b;
            n  = 0;
        } else if ((b & 0xE0) == 0xC0) {
            cp = b & 0x1F;
            n  = 1;
        } else if ((b & 0xF0) == 0xE0) {
            cp = b & 0x0F;
            n  = 2;
        } else if ((b & 0xF8) == 0xF0) {
            cp = b & 0x07;
            n  = 3;
        } else {
            ++p;
            return kReplacementChar;
        }
        ++p;
        for (int i = 0; i < n; ++i) {
            if (p >= end) return kReplacementChar;
            b = static_cast<uint8_t>(*p);
            if ((b & 0xC0) != 0x80) return kReplacementChar;
            cp = (cp << 6) | (b & 0x3F);
            ++p;
        }
        // Canonical minimum codepoint per encoded length — reject overlong
        // forms (the classic security-bypass trick + a DoS surface for FT).
        static constexpr uint32_t kMin[4] = { 0u, 0x80u, 0x800u, 0x10000u };
        if (cp < kMin[n]) return kReplacementChar;
        // UTF-16 surrogate halves are not valid Unicode scalar values; some
        // fonts crash FT_Load_Char on them.
        if (cp >= 0xD800u && cp <= 0xDFFFu) return kReplacementChar;
        // Unicode caps at U+10FFFF.
        if (cp > 0x10FFFFu) return kReplacementChar;
        return cp;
    }

    // Rasterize UTF-8 text to an RGBA8 Image using FreeType.
    // fontData: raw font file bytes; width/height: texture dimensions.
    // Returns nullptr on failure.
    static std::shared_ptr<Image> RenderText(const std::string& fontData, float pointsize,
                                             const std::string& text, i32 width, i32 height,
                                             const std::string& halign, const std::string& valign,
                                             i32 padding);

    // Test-only accessors for the FT_Face LRU cache.  Always available
    // (no #ifdef gate) — cheap and the test target already pokes internals.
    // The first two lock s_ftLibMutex internally; safe to call from any thread.
    // TEST_getLastPixelSize takes the font data pointer used as the cache key;
    // returns 0 if no entry is found.  The signature uses `unsigned int` so
    // the header does not need to drag in <ft2build.h> (FT_UInt is a typedef
    // of unsigned int in every FreeType release we support).
    static std::size_t  TEST_getFaceCacheSize();
    static unsigned int TEST_getLastPixelSize(const void* fontDataPtr);
    static std::size_t  TEST_getFaceCacheCapacity();

    // Test-only accessors for kerning + fallback diagnostics.
    //   TEST_measureLineWidthWithKerning / NoKerning rebuild a throwaway
    //   FT_Face per call (not via the LRU cache) so they do not pollute
    //   cache-size assertions in the FT_Face cache suite.
    //   TEST_reset/getKerningProbeCounter — counts FT_Get_Kerning attempts.
    //   TEST_hostFontHasKerning — wraps FT_HAS_KERNING on the passed font.
    //   TEST_reset/getFallbackProbeCounter — counts CJK fallback consults.
    //   TEST_reset/getMissingGlyphLogCount — counts rate-limited LOG_INFO
    //   firings for missing glyphs in this process.
    static int  TEST_measureLineWidthWithKerning(const std::string& fontData, float pointsize,
                                                 const std::string& line);
    static int  TEST_measureLineWidthNoKerning(const std::string& fontData, float pointsize,
                                               const std::string& line);
    static void TEST_resetKerningProbeCounter();
    static int  TEST_getKerningProbeCount();
    static bool TEST_hostFontHasKerning(const std::string& fontData);
    static void TEST_resetFallbackProbeCounter();
    static int  TEST_getFallbackProbeCount();
    static void TEST_resetMissingGlyphLogCounter();
    static int  TEST_getMissingGlyphLogCount();
    static void TEST_resetLoadGlyphFailLogCounter();
    static int  TEST_getLoadGlyphFailLogCount();
};

} // namespace wallpaper
