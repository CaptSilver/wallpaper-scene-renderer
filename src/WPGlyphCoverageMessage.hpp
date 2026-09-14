#pragma once

#include <cstdio>
#include <string>

namespace wallpaper
{

// Build the rate-limited glyph-coverage LOG_INFO for one RenderText call.
//
// The raster loop hits two distinct outcomes when the primary font lacks a
// codepoint: the CJK fallback face resolves a real glyph, or nothing does
// and .notdef gets drawn. Only the second one is a visible box on the
// wallpaper — conflating them (as the original single "missing" counter
// did) makes the log claim a .notdef box was emitted for text that actually
// rendered fine through the fallback face. Kept as a pure function, header
// side, so the wording can be doctested without a FreeType face.
//
// Returns an empty string when there is nothing to report.
inline std::string BuildGlyphCoverageMessage(int notdefCount, int fallbackCount,
                                             const std::string& text) {
    if (notdefCount <= 0 && fallbackCount <= 0) return {};

    char buf[320];
    if (notdefCount > 0 && fallbackCount > 0) {
        std::snprintf(buf,
                      sizeof(buf),
                      "WPTextRenderer: %d codepoint(s) missing in font (.notdef glyph "
                      "emitted), %d resolved via the CJK fallback face in text \"%.32s\" "
                      "— font may not cover the script",
                      notdefCount,
                      fallbackCount,
                      text.c_str());
    } else if (notdefCount > 0) {
        std::snprintf(buf,
                      sizeof(buf),
                      "WPTextRenderer: %d codepoint(s) missing in font (.notdef glyph "
                      "emitted) in text \"%.32s\" — font may not cover the script",
                      notdefCount,
                      text.c_str());
    } else {
        std::snprintf(buf,
                      sizeof(buf),
                      "WPTextRenderer: %d codepoint(s) missing from the primary font, "
                      "resolved via the CJK fallback face in text \"%.32s\" — font may "
                      "not cover the script",
                      fallbackCount,
                      text.c_str());
    }
    return std::string(buf);
}

} // namespace wallpaper
