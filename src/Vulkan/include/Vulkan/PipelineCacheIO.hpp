#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace wallpaper::vulkan::pipeline_cache_io
{

// Resolves $XDG_CACHE_HOME/wallpaper-scene-renderer/pipeline.cache (or
// $HOME/.cache/... when XDG_CACHE_HOME is unset).  Returns an empty path
// when neither env var is usable — the caller treats that as "no cache".
std::filesystem::path PathFromEnv();

// Vulkan pipeline cache header layout per Vulkan spec 9.6.1:
//   u32 headerLength | u32 headerVersion | u32 vendorID | u32 deviceID | u8[16] cacheUUID
// True iff the blob's header matches the given device's identity.  Mismatch
// means the cache came from a different GPU/driver and must not be passed
// to vkCreatePipelineCache.
bool HeaderMatches(const std::vector<char>& blob, const VkPhysicalDeviceProperties& props);

// Reads the cache file into a byte buffer.  Empty result on miss/IO error
// — both treated as "start with no prior data".
std::vector<char> Read(const std::filesystem::path& path);

// Atomic write via temp file + rename.  Avoids leaving a half-written cache
// on disk if the process is killed mid-write.  Creates parent directories.
bool Write(const std::filesystem::path& path, const void* data, std::size_t bytes);

} // namespace wallpaper::vulkan::pipeline_cache_io
