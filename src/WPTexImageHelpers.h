#pragma once
#include "Type.hpp"
#include "Image.hpp"
#include "Utils/Algorism.h"
#include "Utils/Logging.h"

#include <string>
#include <string_view>

namespace wallpaper
{
namespace teximage_helpers
{

inline TextureFormat ToTexFormate(int type) {
    /*
        type
        RGBA8888 = 0,
        DXT5 = 4,
        DXT3 = 6,
        DXT1 = 7,
        RG88 = 8,
        R8 = 9,
    */
    switch (type) {
    case 0: return TextureFormat::RGBA8;
    case 4: return TextureFormat::BC3;
    case 6: return TextureFormat::BC2;
    case 7: return TextureFormat::BC1;
    case 8: return TextureFormat::RG8;
    case 9: return TextureFormat::R8;
    case 10: return TextureFormat::RGBA16F;
    case 11: return TextureFormat::RG16F;
    case 12: return TextureFormat::BC7;
    case 13: return TextureFormat::R16F;
    case 14: return TextureFormat::BC6H;
    case 15: return TextureFormat::RGB565;
    case 16: return TextureFormat::RGBA1010102;
    default:
        LOG_ERROR("ERROR::ToTexFormate Unkown image type: %d", type);
        return TextureFormat::RGBA8;
    }
}

constexpr std::string_view ALIAS_PREFIX = "_alias_";

inline bool IsAliasTexture(const std::string& name) {
    return name.compare(0, ALIAS_PREFIX.size(), ALIAS_PREFIX) == 0;
}

inline void SetHeaderPow2(ImageHeader& header, i32 mip_0_w, i32 mip_0_h) {
    header.mipmap_pow2   = algorism::IsPowOfTwo((u32)mip_0_w) || algorism::IsPowOfTwo((u32)mip_0_h);
    // Promote to i64 before multiplying — a hostile .tex with INT32_MAX
    // dimensions otherwise overflows the i32 product (UBSAN trip).
    header.mipmap_larger = static_cast<i64>(mip_0_w) * mip_0_h
                         > static_cast<i64>(header.mapWidth) * header.mapHeight;
}

// Replace characters that would break path assembly when the tex name becomes
// part of an on-disk filename.  Reserves `/`, `\`, and `:` — the shape is
// tight so a flipped comparison in the old inline loop produces observably
// different output for any of those separators (killable by tests).
inline char SanitizePathSeparatorChar(char c) {
    if (c == '/' || c == '\\' || c == ':') return '_';
    return c;
}

// Byte size for a tightly-packed RGBA8 (4 bytes/pixel) buffer of w * h
// pixels.  Pulled out so a direct unit test can pin down the arithmetic
// (`w * h * 4`) and kill `*`→`/` mutants that otherwise survive in the
// inline MP4-fallback path.
//
// Returns -1 if w/h are negative or larger than 64K (which already exceeds
// every realistic texture and prevents the i32-product UBSAN trip on
// hostile inputs near INT32_MAX). Caller must check the sentinel.
inline i64 Rgba8ByteSize(i32 w, i32 h) {
    if (w < 0 || h < 0) return -1;
    constexpr i64 kMaxDim = 65536;
    if (w > kMaxDim || h > kMaxDim) return -1;
    return static_cast<i64>(w) * h * 4;
}

} // namespace teximage_helpers
} // namespace wallpaper
