#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>

namespace wallpaper::vulkan
{

// Bounded-retry wrapper for vvk::Fence::Wait at the per-slot frame fence.
// Returns:
//   VK_SUCCESS / VK_SUBOPTIMAL_KHR - fence retired (or sub-success).
//   VK_ERROR_DEVICE_LOST          - retries exhausted (operational timeout
//                                   promoted to terminal so the caller's
//                                   VVK_CHECK_DEVICE_LOST routes through
//                                   the existing device-lost recovery path
//                                   instead of leaking the frame).
//   Any other VkResult            - passthrough from the underlying Wait
//                                   (genuine driver error, not TIMEOUT).
//
// The Vulkan spec treats VK_TIMEOUT as an *operational* result ("GPU isn't
// done yet") - retry is the correct response, not an abort.  3 retries at
// 10s each = 30s total before promotion to DEVICE_LOST.  In normal desktop
// use the GPU is never 10 s behind; under gaming + video encoding +
// wallpaper coexistence, sustained starvation can briefly hit it.
//
// Pure-callable signature (takes std::function<VkResult(uint64_t)> rather
// than a real vvk::Fence&) so the retry policy is unit-testable without a
// Vulkan device.  Production callers wrap fence.Wait in a lambda.
VkResult waitFenceWithRetry(const std::function<VkResult(uint64_t)>& wait_fn, uint64_t wait_ns,
                            int max_retries = 3);

} // namespace wallpaper::vulkan
