#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wallpaper
{
namespace vulkan
{

// Hash + equality contract for VkSamplerCreateInfo dedup in TextureCache.
//
// TextureCache::CreateTex used to build a VkSamplerCreateInfo and call
// device.CreateSampler per texture, leaving each texture owning its own
// VkSampler.  In practice a scene has ~5-15 unique sampler configs
// (LINEAR/NEAREST x CLAMP/REPEAT/MIRROR x anisotropy x maxLod) but 50-200
// textures, so most of those samplers were duplicates.  Hash the meaningful
// fields of VkSamplerCreateInfo; group entries that hash to the same bucket;
// confirm with a field-by-field samplerInfoEqual scan.
//
// Floats are quantized for stable hashing.  pNext, sType, and the never-
// varying fields are still mixed in for robustness (cheap; never the
// limiting factor).  Collisions are bounded to the ~15 distinct configs in
// flight, so the per-lookup scan is O(<=15) on miss-confirm.

inline std::size_t hashSamplerInfo(const VkSamplerCreateInfo& info) noexcept {
    // FNV-1a flavoured mix.  Quantize floats so equality bits round-trip.
    std::size_t h   = 0xcbf29ce484222325ULL;
    auto        mix = [&h](std::uint64_t v) {
        h ^= v;
        h *= 0x100000001b3ULL;
    };
    mix(static_cast<std::uint64_t>(info.flags));
    mix(static_cast<std::uint64_t>(info.magFilter));
    mix(static_cast<std::uint64_t>(info.minFilter));
    mix(static_cast<std::uint64_t>(info.mipmapMode));
    mix(static_cast<std::uint64_t>(info.addressModeU));
    mix(static_cast<std::uint64_t>(info.addressModeV));
    mix(static_cast<std::uint64_t>(info.addressModeW));
    mix(static_cast<std::uint64_t>(info.mipLodBias * 1024.0f));
    mix(static_cast<std::uint64_t>(info.anisotropyEnable));
    mix(static_cast<std::uint64_t>(info.maxAnisotropy * 1024.0f));
    mix(static_cast<std::uint64_t>(info.compareEnable));
    mix(static_cast<std::uint64_t>(info.compareOp));
    mix(static_cast<std::uint64_t>(info.minLod * 1024.0f));
    mix(static_cast<std::uint64_t>(info.maxLod * 1024.0f));
    mix(static_cast<std::uint64_t>(info.borderColor));
    mix(static_cast<std::uint64_t>(info.unnormalizedCoordinates));
    return h;
}

inline bool samplerInfoEqual(const VkSamplerCreateInfo& a, const VkSamplerCreateInfo& b) noexcept {
    return a.flags == b.flags && a.magFilter == b.magFilter && a.minFilter == b.minFilter &&
           a.mipmapMode == b.mipmapMode && a.addressModeU == b.addressModeU &&
           a.addressModeV == b.addressModeV && a.addressModeW == b.addressModeW &&
           a.mipLodBias == b.mipLodBias && a.anisotropyEnable == b.anisotropyEnable &&
           a.maxAnisotropy == b.maxAnisotropy && a.compareEnable == b.compareEnable &&
           a.compareOp == b.compareOp && a.minLod == b.minLod && a.maxLod == b.maxLod &&
           a.borderColor == b.borderColor && a.unnormalizedCoordinates == b.unnormalizedCoordinates;
}

// Templated cache type.  Stored as bucket-vector (hash -> [(info, sampler)])
// so collisions are disambiguated by the samplerInfoEqual scan.  Templated
// on the sampler-handle type so tests can drive it with a plain POD without
// pulling in a live Vulkan device.
template<class Sampler>
using SamplerBucketMap =
    std::unordered_map<std::size_t, std::vector<std::pair<VkSamplerCreateInfo, Sampler>>>;

// Get-or-create lookup helper.  On hit, returns the existing sampler by
// reference; on miss, invokes creator(info) and stores the returned sampler
// in the matching bucket, then returns a reference to the stored entry.
// References stay stable because std::vector entries are pushed-back-only
// (we never erase or reorder mid-scene).
template<class Sampler, class Creator, class Hasher>
Sampler& findOrCreateSamplerWithHash(SamplerBucketMap<Sampler>& cache,
                                     const VkSamplerCreateInfo& info, Creator&& creator,
                                     Hasher&& hasher) {
    const std::size_t h      = hasher(info);
    auto&             bucket = cache[h];
    for (auto& entry : bucket) {
        if (samplerInfoEqual(entry.first, info)) {
            return entry.second;
        }
    }
    bucket.emplace_back(info, creator(info));
    return bucket.back().second;
}

template<class Sampler, class Creator>
Sampler& findOrCreateSampler(SamplerBucketMap<Sampler>& cache, const VkSamplerCreateInfo& info,
                             Creator&& creator) {
    return findOrCreateSamplerWithHash(
        cache, info, std::forward<Creator>(creator), &hashSamplerInfo);
}

} // namespace vulkan
} // namespace wallpaper
