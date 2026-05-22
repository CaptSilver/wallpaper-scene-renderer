#pragma once
#include <cstdint>
#include <vulkan/vulkan.h> // VkDeviceSize (matches the sibling pure helper TexFormatVk.hpp)

namespace wallpaper
{
namespace vulkan
{

// Half-open byte interval [lo, hi) written into a staging buffer since the
// last reset().  Per-frame writes are merged into one convex hull: the
// staging buffer is bump-allocated and never cleared between frames, so every
// byte inside the hull holds a valid (last-written) value and a single
// contiguous flush+copy over the hull is byte-identical to copying the whole
// buffer, even when individual writes leave gaps inside the hull.
struct DirtySpan {
    VkDeviceSize lo { 0 };
    VkDeviceSize hi { 0 }; // hi == lo  <=>  empty

    void reset() {
        lo = 0;
        hi = 0;
    }
    bool empty() const { return hi <= lo; }

    void extend(VkDeviceSize offset, VkDeviceSize size) {
        if (size == 0) return;
        const VkDeviceSize end = offset + size;
        if (empty()) {
            lo = offset;
            hi = end;
            return;
        }
        if (offset < lo) lo = offset;
        if (end > hi) hi = end;
    }

    struct Range {
        VkDeviceSize offset;
        VkDeviceSize size;
    };
    // Flush/copy range: round lo down / hi up to the host non-coherent atom,
    // clamp to [0, cap] (cap = buffer req_size) so VMA never sees
    // offset+size > allocationSize.  Same range serves the device-side
    // VkBufferCopy (no atom requirement; the <atom over-copy is harmless,
    // still-valid staging data).  {0,0} when empty.
    Range alignedRange(VkDeviceSize atom, VkDeviceSize cap) const {
        if (empty()) return { 0, 0 };
        const VkDeviceSize a   = atom == 0 ? 1 : atom;
        VkDeviceSize       off = (lo / a) * a;
        VkDeviceSize       end = ((hi + a - 1) / a) * a;
        if (off > cap) off = cap;
        if (end > cap) end = cap;
        if (end < off) end = off;
        return { off, end - off };
    }
};

} // namespace vulkan
} // namespace wallpaper
