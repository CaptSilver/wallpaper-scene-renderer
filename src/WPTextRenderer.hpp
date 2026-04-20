#pragma once
#include "Image.hpp"
#include "Core/Literals.hpp"
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

    static void Init();
    static void Shutdown();

    // Rasterize UTF-8 text to an RGBA8 Image using FreeType.
    // fontData: raw font file bytes; width/height: texture dimensions.
    // Returns nullptr on failure.
    static std::shared_ptr<Image> RenderText(const std::string& fontData, float pointsize,
                                             const std::string& text, i32 width, i32 height,
                                             const std::string& halign, const std::string& valign,
                                             i32 padding);
};

} // namespace wallpaper
