#pragma once
// Conditional texture variants: a .tex can carry alternative versions of its
// image that a user property switches between (a "Face Styles" combo picking
// one of several faces). The base image is variant 0; every other variant is
// stored per mip as patch rectangles that replace part of the base mip.
// Pure helpers, kept out of the parser so the arithmetic is testable.

#include "Image.hpp"
#include "Type.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wallpaper::texvariant
{

// Answers "does user property `property` currently equal `value`?".
using Selector = std::function<bool(const std::string& property, const std::string& value)>;

// Side of the square a format stores as one unit: 4 for block-compressed
// formats, 1 for per-pixel ones.
inline i32 BlockDim(TextureFormat fmt) {
    switch (fmt) {
    case TextureFormat::BC1:
    case TextureFormat::BC2:
    case TextureFormat::BC3:
    case TextureFormat::BC6H:
    case TextureFormat::BC7: return 4;
    default: return 1;
    }
}

inline i64 BytesPerUnit(TextureFormat fmt) {
    switch (fmt) {
    case TextureFormat::BC1: return 8;
    case TextureFormat::BC2:
    case TextureFormat::BC3:
    case TextureFormat::BC6H:
    case TextureFormat::BC7: return 16;
    case TextureFormat::RGB8: return 3;
    case TextureFormat::RGBA8: return 4;
    case TextureFormat::RG8: return 2;
    case TextureFormat::R8: return 1;
    case TextureFormat::RGBA16F: return 8;
    case TextureFormat::RG16F: return 4;
    case TextureFormat::R16F: return 2;
    case TextureFormat::RGB565: return 2;
    case TextureFormat::RGBA1010102: return 4;
    }
    return 0;
}

// Bytes a tightly packed w x h region occupies in `fmt`, rounding partial
// blocks up. -1 for a non-positive or implausible size.
inline i64 RegionBytes(TextureFormat fmt, i32 w, i32 h) {
    constexpr i32 kMaxDim = 65536;
    if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim) return -1;
    const i64 bd = BlockDim(fmt);
    return ((w + bd - 1) / bd) * ((h + bd - 1) / bd) * BytesPerUnit(fmt);
}

// Reads one header record's JSON, {"condition":{"condition":V,"name":P}}.
// A non-string V is compared by its JSON text, as WPUserProperties does for
// layer visibility conditions.
inline std::optional<TexVariantCondition> ParseCondition(i32 index, std::string_view json) {
    if (index <= 0) return std::nullopt;
    auto j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (! j.is_object() || ! j.contains("condition")) return std::nullopt;
    const auto& c = j.at("condition");
    if (! c.is_object() || ! c.contains("name") || ! c.contains("condition")) return std::nullopt;
    if (! c.at("name").is_string()) return std::nullopt;
    const auto& v = c.at("condition");
    return TexVariantCondition { index,
                                 c.at("name").get<std::string>(),
                                 v.is_string() ? v.get<std::string>() : v.dump() };
}

// The variant the current user properties select: the first condition that
// holds, or 0 (the base image) when none does.
inline i32 SelectVariant(const std::vector<TexVariantCondition>& conds, const Selector& holds) {
    if (! holds) return 0;
    for (const auto& c : conds)
        if (holds(c.property, c.value)) return c.index;
    return 0;
}

// Copies a w x h patch over the region at (x, y) of a mipW x mipH mip. The
// rectangle must lie inside the mip and start on a block boundary; its far
// edge may stop short of a block only at the mip's own edge. Returns false
// and leaves the mip untouched when anything does not fit.
inline bool BlitPatch(TextureFormat fmt, uint8_t* mip, isize mipSize, i32 mipW, i32 mipH,
                      const uint8_t* patch, isize patchSize, i32 x, i32 y, i32 w, i32 h) {
    const i32 bd = BlockDim(fmt);
    if (mip == nullptr || patch == nullptr) return false;
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return false;
    if (x % bd != 0 || y % bd != 0) return false;
    if ((i64)x + w > mipW || (i64)y + h > mipH) return false;
    if ((w % bd != 0 && x + w != mipW) || (h % bd != 0 && y + h != mipH)) return false;
    if (RegionBytes(fmt, mipW, mipH) != mipSize) return false;
    if (RegionBytes(fmt, w, h) != patchSize) return false;

    const i64 dstStride = RegionBytes(fmt, mipW, bd);
    const i64 rowBytes  = RegionBytes(fmt, w, bd);
    const i64 dstX      = (x / bd) * BytesPerUnit(fmt);
    const i64 rows      = (h + bd - 1) / bd;
    for (i64 r = 0; r < rows; r++) {
        std::memcpy(mip + (y / bd + r) * dstStride + dstX, patch + r * rowBytes, (size_t)rowBytes);
    }
    return true;
}

} // namespace wallpaper::texvariant
