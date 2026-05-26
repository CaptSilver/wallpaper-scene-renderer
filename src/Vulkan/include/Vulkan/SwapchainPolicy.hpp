#pragma once
#include <vulkan/vulkan.h>

namespace wallpaper
{
namespace vulkan
{

// Classify the VkResult of an Acquire/Present so the swapchain owner
// knows whether to (a) proceed normally, (b) recreate the swapchain on
// the next frame, or (c) fail the renderer outright.
enum class SwapResult { Ok, NeedsRecreate, Fatal };

inline SwapResult classifySwapResult(VkResult r) noexcept {
    switch (r) {
        case VK_SUCCESS:                return SwapResult::Ok;
        case VK_SUBOPTIMAL_KHR:         return SwapResult::Ok;
        case VK_ERROR_OUT_OF_DATE_KHR:  return SwapResult::NeedsRecreate;
        case VK_ERROR_SURFACE_LOST_KHR: return SwapResult::NeedsRecreate;
        default:                        return SwapResult::Fatal;
    }
}

} // namespace vulkan
} // namespace wallpaper
