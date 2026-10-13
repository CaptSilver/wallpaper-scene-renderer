#pragma once
#include <cstddef>
#include <string>
#include <string_view>

#include "Type.hpp"

namespace wallpaper
{

// Stock shaders read a texture's storage format from a TEX<n>FORMAT define and
// compare it against the FORMAT_* table in common_fragment.h.  It picks which
// channels hold a normal map's X and Y (slot 1), whether a toon gradient is
// single-channel (slot 4), and fur4 passes TEX8FORMAT to a function, so a
// missing define is a compile error there, not just a wrong branch.
inline std::string TexFormatComboName(std::size_t slot) {
    return "TEX" + std::to_string(slot) + "FORMAT";
}

// The FORMAT_* name for a texture format, or empty when the stock table has no
// entry for it.  An undefined TEX<n>FORMAT reads as 0, FORMAT_RGBA8888.
inline std::string_view TexFormatDefine(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8: return "FORMAT_RGBA8888";
    case TextureFormat::RGB8: return "FORMAT_RGB888";
    case TextureFormat::RGB565: return "FORMAT_RGB565";
    case TextureFormat::BC1: return "FORMAT_DXT1";
    case TextureFormat::BC2: return "FORMAT_DXT3";
    case TextureFormat::BC3: return "FORMAT_DXT5";
    case TextureFormat::BC7: return "FORMAT_BC7";
    case TextureFormat::RG8: return "FORMAT_RG88";
    case TextureFormat::R8: return "FORMAT_R8";
    case TextureFormat::RG16F: return "FORMAT_RG1616F";
    case TextureFormat::R16F: return "FORMAT_R16F";
    case TextureFormat::RGBA16F:
    case TextureFormat::BC6H:
    case TextureFormat::RGBA1010102: return {};
    }
    return {};
}

// Records slot `slot`'s format in a shader's combo map.  Templated on the map so
// this header stays free of the shader parser's includes.
template<typename ComboMap>
void ApplyTexFormatCombo(ComboMap& combos, std::size_t slot, TextureFormat format) {
    const std::string_view define = TexFormatDefine(format);
    if (! define.empty()) combos[TexFormatComboName(slot)] = std::string(define);
}

} // namespace wallpaper
