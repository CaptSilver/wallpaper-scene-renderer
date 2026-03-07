#pragma once
#include "Image.hpp"
#include "Core/Literals.hpp"
#include <memory>
#include <string>

namespace wallpaper
{

class WPTextRenderer {
public:
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
