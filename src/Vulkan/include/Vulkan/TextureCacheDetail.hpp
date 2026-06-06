#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
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

// Parse WEK_TEXCACHE_QUERY_CAP-style env override into a uint32_t soft cap.
// Returns the default when env is null, empty, malformed, or out of the
// accepted [min_cap, max_cap] band.  Out-of-band values must fall back to
// the default — a cap too small thrashes the pool (eviction every frame),
// a cap too large defeats the purpose entirely.  Bounds + default are
// caller-supplied so the helper stays testable in isolation.
//
// `env` mirrors std::getenv()'s return shape (NUL-terminated cstr or
// nullptr); pass a closure in tests, std::getenv in production.
inline std::uint32_t parseQueryCapEnv(const char* env, std::uint32_t default_cap,
                                      std::uint32_t min_cap = 8,
                                      std::uint32_t max_cap = 4096) {
    if (env == nullptr || env[0] == '\0') return default_cap;
    char*               endp = nullptr;
    const unsigned long val  = std::strtoul(env, &endp, 10);
    if (endp == nullptr || *endp != '\0') return default_cap;
    if (val < min_cap || val > max_cap) return default_cap;
    return static_cast<std::uint32_t>(val);
}

// Choose which non-persist query-tex entries to evict so the pool shrinks
// back to <= soft_cap.  Pure function — operates on a parallel ticks/persist
// view of the cache so it can be unit-tested without a live VkDevice or VMA
// allocator.
//
// `lru_ticks[i]` is QueryTex[i].lru_tick; `persist[i]` is QueryTex[i].persist.
// Returns the indices of victims, sorted DESCENDING by index — callers walk
// the result in order and erase by index without needing to renumber.
//
// Eviction policy: oldest-non-persist-first.  Persist=true entries are never
// touched; if every entry is persist=true, the result is empty and the pool
// stays above the cap (intentional — the cap is soft).
inline std::vector<std::size_t> selectEvictionVictims(std::span<const std::uint64_t> lru_ticks,
                                                     std::span<const std::uint8_t>  persist,
                                                     std::size_t soft_cap) {
    if (lru_ticks.size() <= soft_cap) return {};
    if (lru_ticks.size() != persist.size()) return {};

    // Collect indices of non-persist entries, paired with their lru_tick so
    // we can sort by recency without sorting the original pool in-place.
    std::vector<std::pair<std::uint64_t, std::size_t>> cold;
    cold.reserve(lru_ticks.size());
    for (std::size_t i = 0; i < lru_ticks.size(); ++i) {
        if (! persist[i]) cold.emplace_back(lru_ticks[i], i);
    }
    if (cold.empty()) return {};

    // Sort ascending by lru_tick — oldest first.
    std::sort(cold.begin(), cold.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    const std::size_t       overflow = lru_ticks.size() - soft_cap;
    const std::size_t       take     = std::min(overflow, cold.size());
    std::vector<std::size_t> victims;
    victims.reserve(take);
    for (std::size_t i = 0; i < take; ++i) victims.push_back(cold[i].second);

    // Erase-by-index callers want descending order so earlier erases don't
    // invalidate later indices.
    std::sort(victims.begin(), victims.end(), std::greater<std::size_t>());
    return victims;
}

// Combine each query-tex's explicit persist flag with current-generation
// liveness into the effective persist mask fed to selectEvictionVictims.  A
// query-tex requested during the current render-graph generation is a LIVE
// render target — its image backs a framebuffer bound by CustomShaderPass, so
// evicting it is a use-after-free however cold its lru_tick looks.  Entry i is
// protected iff it is already persist OR its last_gen == current_gen; only
// prior-generation (previous-scene) entries stay eligible for reclaim.  Pure
// so it is unit-testable without a live VkDevice.  Size mismatch → empty
// (defensive; selectEvictionVictims then also returns empty).
inline std::vector<std::uint8_t> effectiveEvictionPersist(std::span<const std::uint8_t>  persist,
                                                          std::span<const std::uint64_t> last_gen,
                                                          std::uint64_t current_gen) {
    if (persist.size() != last_gen.size()) return {};
    std::vector<std::uint8_t> out(persist.size());
    for (std::size_t i = 0; i < persist.size(); ++i) {
        out[i] = (persist[i] || last_gen[i] == current_gen) ? std::uint8_t { 1 } : std::uint8_t { 0 };
    }
    return out;
}

} // namespace detail
} // namespace vulkan
} // namespace wallpaper
