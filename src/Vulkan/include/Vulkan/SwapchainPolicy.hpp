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

// The size the swapchain is created at.  The driver's currentExtent is the
// answer whenever it is one the driver will actually accept -- a surface that
// declines to name a size reports 0xFFFFFFFF (Wayland) or 0 (mid-teardown),
// and some drivers report a currentExtent outside their own advertised range,
// which is what made vkCreateSwapchainKHR fail on resize.  In those cases fall
// back to the extent the caller asked for, clamped per axis.
inline VkExtent2D chooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& caps,
                                        VkExtent2D                      requested) noexcept {
    const VkExtent2D min  = caps.minImageExtent;
    const VkExtent2D max  = caps.maxImageExtent;
    const VkExtent2D curr = caps.currentExtent;

    // A zero axis is checked on its own rather than left to the min test:
    // the spec floors minImageExtent at 1, but a driver reporting 0 there
    // would otherwise let a degenerate size through.
    const bool usable = curr.width != 0 && curr.height != 0 && curr.width >= min.width &&
                        curr.width <= max.width && curr.height >= min.height &&
                        curr.height <= max.height;
    if (usable) return curr;

    return VkExtent2D { std::clamp(requested.width, min.width, max.width),
                        std::clamp(requested.height, min.height, max.height) };
}

// One image over the surface minimum, so there is always one to render into
// while the driver holds the rest.  maxImageCount == 0 means "no limit".
inline uint32_t chooseSwapchainImageCount(const VkSurfaceCapabilitiesKHR& caps) noexcept {
    const uint32_t wanted = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && wanted > caps.maxImageCount) return caps.maxImageCount;
    return wanted;
}

// Where the final composite lands.  The renderer either owns a window surface
// (on-screen swapchain) or exports an offscreen swapchain the host samples as
// a dma-buf.  The exported one has to allocate three external images, which
// fails on a GPU that has just reset or has run out of VRAM, so "offscreen"
// alone does not imply there is anything to present into.
enum class PresentTarget
{
    Surface,           // present through the on-screen swapchain
    ExportedOffscreen, // present into the dma-buf-exported swapchain
    None,              // offscreen was asked for and could not be created
};

// None is a hard stop, not a degraded mode: every downstream user of the
// exported swapchain (final-pass format, per-frame image acquire) dereferences
// it, so the renderer must fail init and let the caller retry.
constexpr PresentTarget classifyPresentTarget(bool with_surface,
                                              bool ex_swapchain_created) noexcept {
    if (with_surface) return PresentTarget::Surface;
    return ex_swapchain_created ? PresentTarget::ExportedOffscreen : PresentTarget::None;
}

// Formats for the exported offscreen swapchain -- the one whose images the
// host imports as dma-bufs.  HDR output wants 16-bit float; everything else
// gets the 8-bit format.
inline constexpr VkFormat kExSwapchainSdrFormat = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat kExSwapchainHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

constexpr VkFormat exSwapchainFormat(bool hdr_output) noexcept {
    return hdr_output ? kExSwapchainHdrFormat : kExSwapchainSdrFormat;
}

// Formats to try for the exported swapchain, best first.  A driver is free
// to refuse 16-bit float for an exportable colour attachment, and turning
// HDR on rebuilds the whole renderer -- without a second candidate the
// refusal fails init and the wallpaper stays blank until the user toggles
// the setting back.  8-bit is what the SDR path already runs on, so it is
// the one substitution worth making; a caller naming anything else knows
// what it wants and gets it or nothing.
inline std::vector<VkFormat> exSwapchainFormatCandidates(VkFormat preferred) {
    if (preferred == kExSwapchainHdrFormat) return { kExSwapchainHdrFormat, kExSwapchainSdrFormat };
    return { preferred };
}

// Whether the driver advertises everything the exported images are used for:
// rendered into, sampled back for the final composite, and blitted to.  This
// is the cheap half of the question -- whether the memory can actually be
// exported is only answered by trying, so a caller still walks the candidate
// list on a creation failure.
constexpr bool exSwapchainFormatUsable(const VkFormatProperties& props,
                                       VkImageTiling             tiling) noexcept {
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const VkFormatFeatureFlags     have =
        tiling == VK_IMAGE_TILING_LINEAR ? props.linearTilingFeatures : props.optimalTilingFeatures;
    return (have & required) == required;
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
