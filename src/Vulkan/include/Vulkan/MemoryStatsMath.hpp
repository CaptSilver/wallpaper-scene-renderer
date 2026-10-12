#pragma once
#include <cstdint>

// The arithmetic behind Device::LogMemoryStats, kept apart from the VMA types
// so the doctest suite can check it without a device.
namespace wallpaper
{
namespace vulkan
{

constexpr double kBytesPerMb = 1024.0 * 1024.0;

inline double BytesToMb(std::uint64_t bytes) { return static_cast<double>(bytes) / kBytesPerMb; }

// One VMA stat line in megabytes.  `used` is what live allocations occupy and
// `unused` the free space inside blocks VMA already reserved, so their sum is
// the block total.
struct MemoryStatsMb {
    double used { 0 }, unused { 0 }, block_total { 0 }, unused_range_max { 0 }, alloc_max { 0 };
};

inline MemoryStatsMb MemoryStatsMbOf(std::uint64_t used_bytes, std::uint64_t unused_bytes,
                                     std::uint64_t unused_range_max_bytes,
                                     std::uint64_t alloc_max_bytes) {
    return { BytesToMb(used_bytes),
             BytesToMb(unused_bytes),
             BytesToMb(used_bytes + unused_bytes),
             BytesToMb(unused_range_max_bytes),
             BytesToMb(alloc_max_bytes) };
}

} // namespace vulkan
} // namespace wallpaper
