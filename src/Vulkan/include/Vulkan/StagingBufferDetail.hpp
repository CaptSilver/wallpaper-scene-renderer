#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace wallpaper
{
namespace vulkan
{
namespace detail
{

// MEM3 pure helper: copy the staging payload from src (old map) to dst
// (new map), copying min(src_size, dst_size) bytes.  Extracted from
// StagingBuffer::increaseBuf for unit-testability — the rest of
// increaseBuf manages Vulkan/VMA lifetimes which doctest can't exercise
// without a device.
inline void copyStagingPayload(const uint8_t* src, std::size_t src_size, uint8_t* dst,
                               std::size_t dst_size) {
    std::memcpy(dst, src, std::min(src_size, dst_size));
}

// Half-open staging-slot range [first, last) a write must land in.
struct StagingSlotRange {
    std::size_t first;
    std::size_t last;
};

// Resolve which staging slots a write targets.  The staging buffer is
// N-buffered for frames-in-flight (one slot per in-flight frame).  A value
// that must persist across slot switches — prepare-time defaults and runtime
// constValue re-uploads (e.g. a SceneScript day/night grade) — broadcasts to
// EVERY slot; an ordinary per-frame write touches only the current slot.
// Writing a must-persist value to a single slot leaves the other slot holding
// the stale value, which the renderer then shows on alternate frames: a
// per-frame-in-flight A/B flicker.  Pure so doctest can pin the decision
// without a Vulkan device.
inline StagingSlotRange stagingWriteSlotRange(std::size_t slot_count, std::size_t current_slot,
                                              bool broadcast) {
    if (broadcast) return { 0, slot_count };
    return { current_slot, current_slot + 1 };
}

// MEM3 grow simulator.  Models the data-movement shape of
// StagingBuffer::increaseBuf with a buffer-allocator callback so it can
// be unit-tested under an allocation counter (no Vulkan device needed).
//
// The "direct" parameter selects the algorithmic shape:
//   - direct=false (old): allocate a temp buffer of newsize, copy
//     old→temp, free old, allocate new, copy temp→new.  Peak transient
//     extra allocation = newsize bytes (the temp).
//   - direct=true  (new): allocate new, copy old→new directly, free old.
//     Peak transient extra allocation = 0 bytes (no temp).
//
// alloc_cb(n) returns a pointer to n freshly-allocated bytes.
// free_cb(p) frees a pointer previously returned by alloc_cb.
// Returns the new buffer's pointer.  Caller owns the returned pointer
// and the input old buffer (frees them both when done; this helper only
// frees the temp).
template<typename AllocCb, typename FreeCb>
inline uint8_t* growStagingPayload(uint8_t* old_buf, std::size_t old_size, std::size_t newsize,
                                   bool direct, AllocCb alloc_cb, FreeCb free_cb) {
    if (direct) {
        // New path: allocate new, copy old→new directly.  No temp.
        uint8_t* new_buf = alloc_cb(newsize);
        copyStagingPayload(old_buf, old_size, new_buf, newsize);
        return new_buf;
    } else {
        // Old path: allocate temp of newsize, copy old→temp, free old,
        // allocate new, copy temp→new.  Temp is the ≈3× peak driver.
        uint8_t* tmp = alloc_cb(newsize);
        copyStagingPayload(old_buf, old_size, tmp, newsize);
        // (Caller would free old_buf here in production; the helper does
        //  not free old to keep ownership symmetric with the direct path.)
        uint8_t* new_buf = alloc_cb(newsize);
        copyStagingPayload(tmp, newsize, new_buf, newsize);
        free_cb(tmp);
        return new_buf;
    }
}

} // namespace detail
} // namespace vulkan
} // namespace wallpaper
