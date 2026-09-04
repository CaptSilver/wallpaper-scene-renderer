#pragma once

#include <functional>

namespace wallpaper::vulkan
{

// The out-of-date step of one swapchain frame, in the order the objects demand
// it.
//
// Rebuilding the chain destroys every swapchain image view, and anything
// holding a framebuffer keyed on one of those views is stale the moment that
// happens.  The allocator is free to hand the same VkImageView value straight
// back -- N views freed, N of the same size requested -- so a cache that is
// never told does not merely leak: it *hits*, and the pass binds a framebuffer
// whose attachment is a destroyed view sized to the pre-resize extent while
// renderArea and viewport carry the new one.  Invalidation is therefore part of
// the step, not something the caller may forget.
//
// Plain callables rather than Vulkan handles: no headless environment can
// create a VkSurfaceKHR, so this is the only way the ordering gets tested.
// Same trick as runFrameFenceCycle next door.
struct SwapchainRecreateOps {
    // Waits for in-flight work to stop reading the old swapchain images.
    // false = device lost; nothing below is safe to run.
    std::function<bool()> wait_device_idle;
    // Swapchain::Recreate.  false = the chain could not be rebuilt.
    std::function<bool()> recreate;
    // Drops everything keyed on the image views the recreate destroyed.
    std::function<void()> invalidate_view_caches;
    // Replaces the half-signaled image-available semaphore the failed acquire
    // left behind -- there is no way to unsignal one.
    std::function<bool()> replace_acquire_semaphore;
};

// true when the swapchain is usable again and the caller may clear its
// pending-recreate flag; false leaves the flag set so the next frame retries.
// Either way the current frame is abandoned -- the recreate consumed it.
inline bool runSwapchainRecreate(const SwapchainRecreateOps& ops) {
    if (! ops.wait_device_idle()) return false;

    const bool recreated = ops.recreate();
    // Unconditional, and deliberately not folded into the success path: the
    // rebuild tears the old images and views down before it creates their
    // replacements, so a recreate that fails leaves the views just as dead as
    // one that succeeds.
    ops.invalidate_view_caches();
    if (! recreated) return false;

    return ops.replace_acquire_semaphore();
}

} // namespace wallpaper::vulkan
