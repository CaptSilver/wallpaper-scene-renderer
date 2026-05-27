#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

namespace wallpaper
{
namespace vulkan
{
namespace detail
{

// Cumulative-offset table for packing the mip levels of a single texture
// into one contiguous staging buffer.  offsets[i] is the byte position at
// which mip i starts in the packed buffer.
//
// Alignment: VkBufferImageCopy::bufferOffset must be a multiple of the
// format's texel block size (1 for uncompressed RGBA8/R8/RG8/RGBA16F etc.;
// 8 for BC1/BC4; 16 for BC2/BC3/BC5/BC6H/BC7).  The helper rounds each
// per-mip starting offset up to blockSize.  Default blockSize = 1 leaves
// the cumulative sum untouched (uncompressed fast path).
//
// Returns an offsets table the same length as mip_sizes.  Empty input
// yields an empty table.
inline std::vector<std::size_t> mipOffsets(std::span<const std::size_t> mip_sizes,
                                           std::size_t                  blockSize = 1) {
    std::vector<std::size_t> offsets(mip_sizes.size());
    std::size_t              cumul = 0;
    for (std::size_t j = 0; j < mip_sizes.size(); ++j) {
        if (blockSize > 1) {
            cumul = (cumul + blockSize - 1) / blockSize * blockSize;
        }
        offsets[j] = cumul;
        cumul += mip_sizes[j];
    }
    return offsets;
}

// Total bytes required to hold all mip levels (last offset + last mip
// size, with the final block-size round-up already applied by mipOffsets
// for any earlier mip; the trailing mip needs no further rounding because
// no offset follows it).
inline std::size_t packedTotalBytes(std::span<const std::size_t> mip_sizes,
                                    std::span<const std::size_t> offsets) {
    if (mip_sizes.empty()) return 0;
    return offsets.back() + mip_sizes.back();
}

// Bytes per texel block for the formats this engine actually loads (see
// src/Vulkan/include/Vulkan/TexFormatVk.hpp).  For uncompressed formats
// the engine fills mip sizes as width*height*bytes_per_pixel, all of
// which are already 4-aligned for the worst-case (RGBA8 / R32F) — return
// 1 so the offset table tracks the raw cumulative sum.  For block-
// compressed formats return the actual block byte count so the per-mip
// VkBufferImageCopy::bufferOffset honours
// VUID-vkCmdCopyBufferToImage-bufferOffset-01558.
inline std::size_t bytesPerBlockForFormat(VkFormat fmt) {
    switch (fmt) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK:
        return 8;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC5_UNORM_BLOCK:
    case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
        return 16;
    default:
        return 1;
    }
}

// Pack a sequence of mip-source byte arrays into a destination buffer at
// the offsets computed by mipOffsets().  Mirrors the memcpy loop in
// TextureCache::CreateTex(Image&) so the loop can be exercised without a
// Vulkan device.
//
// Callers pass dst as a pre-sized buffer (at least packedTotalBytes() bytes).
inline void packMipsIntoBuffer(std::span<const std::pair<const std::uint8_t*, std::size_t>> mips,
                               std::span<const std::size_t>                                 offsets,
                               std::uint8_t*                                                dst) {
    for (std::size_t j = 0; j < mips.size(); ++j) {
        std::memcpy(dst + offsets[j], mips[j].first, mips[j].second);
    }
}

} // namespace detail
} // namespace vulkan
} // namespace wallpaper
