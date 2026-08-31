#pragma once
#include <vulkan/vulkan.h>

#include <algorithm>
#include <vector>

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


// Policy by which the swapchain picks among supported present modes.
// Auto is the default and selects based on target_fps vs output_refresh_hz.
// The four explicit modes are fall-backs the user can pin from the
// SettingPage ComboBox.  Each falls back to FIFO when the preferred mode
// is not advertised by the surface — FIFO is guaranteed by the Vulkan spec.
enum class PresentModePolicy
{
    Auto        = 0, // pick based on target_fps vs output_refresh_hz ratio
    Fifo        = 1, // strict vsync
    FifoRelaxed = 2, // vsync but allow late-frame catch-up (sub-refresh fps smoothing)
    Mailbox     = 3, // unthrottled, drop frames (low-latency, may waste GPU)
    Immediate   = 4, // no vsync at all (rare; mostly benchmarking)
};

// Highest valid enumerator above; keep in step when adding a policy.
inline constexpr int kMaxPresentModePolicy = 4;

// An int arriving from a CLI flag or a QML property has not been range
// checked.  Casting one straight into the enum yields an enumerator no
// switch arm below handles, so validate before converting.
constexpr bool IsValidPresentModePolicy(int v) noexcept {
    return v >= 0 && v <= kMaxPresentModePolicy;
}

// Out-of-range degrades to Auto — the same thing an unset setting does.
constexpr PresentModePolicy ToPresentModePolicy(int v) noexcept {
    return IsValidPresentModePolicy(v) ? static_cast<PresentModePolicy>(v)
                                       : PresentModePolicy::Auto;
}

// Explicit non-Auto policies (Fifo / FifoRelaxed / Mailbox / Immediate)
// pin the requested mode, falling back to FIFO when the surface does
// not advertise it (e.g. MAILBOX on some Wayland surfaces, IMMEDIATE on
// frame-pacing-strict drivers).
inline VkPresentModeKHR pickPresentMode(const std::vector<VkPresentModeKHR>& supported,
                                        PresentModePolicy policy,
                                        int               target_fps,
                                        int               output_refresh_hz) {
    auto has = [&](VkPresentModeKHR m) {
        return std::find(supported.begin(), supported.end(), m) != supported.end();
    };

    switch (policy) {
    case PresentModePolicy::Auto: {
        if (target_fps > output_refresh_hz * 11 / 10 && has(VK_PRESENT_MODE_MAILBOX_KHR))
            return VK_PRESENT_MODE_MAILBOX_KHR;
        if (target_fps < output_refresh_hz * 9 / 10 && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
            return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    case PresentModePolicy::Mailbox:
        return has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                                                : VK_PRESENT_MODE_FIFO_KHR;
    case PresentModePolicy::FifoRelaxed:
        return has(VK_PRESENT_MODE_FIFO_RELAXED_KHR) ? VK_PRESENT_MODE_FIFO_RELAXED_KHR
                                                     : VK_PRESENT_MODE_FIFO_KHR;
    case PresentModePolicy::Immediate:
        return has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                  : VK_PRESENT_MODE_FIFO_KHR;
    case PresentModePolicy::Fifo:
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

} // namespace vulkan
} // namespace wallpaper
