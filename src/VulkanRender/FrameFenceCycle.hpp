#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>

namespace wallpaper::vulkan
{

// The fence-bearing section of one swapchain frame: wait on the slot's frame
// fence, record, then reset that fence and submit with it.
//
// The ordering is the whole point.  A frame records into slot
// frame_index % frames_in_flight, and the fence living in that slot is what
// the *next* frame to land there waits on.  Recording can still give the
// frame up after the wait but before the submit — the swapchain needs
// recreating, the acquire came back OUT_OF_DATE from a window resize, the
// acquire failed outright — and an abandoned frame leaves the index where it
// was, so the very next frame re-picks the same slot.  Reset that slot's
// fence on the way out and there is nothing left to retire it: the next wait
// burns its whole retry budget and comes back as a fabricated
// VK_ERROR_DEVICE_LOST.  Recovery then rebuilds the renderer and replays the
// settings that asked for the recreate, so it re-arms immediately and the
// viewer never presents a frame.
//
// Reset and index advance both belong to the submit, and keeping them here
// keeps them from drifting apart from it again.
//
// Plain callables rather than Vulkan handles: no headless environment can
// create a VkSurfaceKHR, so this is the only way the ordering gets tested.
// Same trick as waitFenceWithRetry next door.
struct FrameFenceCycleOps {
    // Waits for the slot's previous submission to retire.
    std::function<VkResult()> wait_fence;
    // Everything between the wait and the submit: swapchain recreate check,
    // image acquire, command-buffer recording.  false = frame abandoned.
    std::function<bool()> record;
    // vkResetFences on the slot's frame fence.
    std::function<VkResult()> reset_fence;
    // vkQueueSubmit taking the slot's frame fence.
    std::function<VkResult()> submit;
};

// Returns the first failing VkResult from wait/reset/submit for the caller to
// route through its device-lost check, VK_SUCCESS otherwise.  `submitted`
// tells a successful caller whether the frame actually reached the submit —
// false means it was abandoned and there is nothing to present.  frame_index
// advances only on a submit.
VkResult runFrameFenceCycle(const FrameFenceCycleOps& ops, std::uint64_t& frame_index,
                            bool& submitted);

} // namespace wallpaper::vulkan
