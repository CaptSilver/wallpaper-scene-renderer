#pragma once
#include "Vulkan/SwapchainPolicy.hpp"
#include "Instance.hpp"
#include <span>

namespace wallpaper
{
namespace vulkan
{
struct ImageParameters;
struct VmaImageParameters;

// PresentModePolicy + pickPresentMode live in SwapchainPolicy.hpp.

class Device;
class Swapchain {
public:
    static bool                      Create(Device&, VkSurfaceKHR, VkExtent2D, Swapchain&);
    // Recreate this swapchain in place after VK_ERROR_OUT_OF_DATE_KHR
    // (typical Wayland resize / output unplug). The previous handle is
    // passed via VkSwapchainCreateInfoKHR::oldSwapchain so the driver
    // can reuse resources where possible. Returns false on hard failure;
    // caller should treat as device-lost.
    bool                             Recreate(Device&, VkSurfaceKHR, VkExtent2D);
    const vvk::SwapchainKHR&         handle() const;
    VkFormat                         format() const;
    VkExtent2D                       extent() const;
    VkPresentModeKHR                 presentMode() const;
    std::span<const ImageParameters> images() const;

    // User-configurable inputs to pickPresentMode().  Take effect at the next
    // Create() or Recreate().  VulkanRender::setSwapchainPresentPolicy applies
    // them mid-session and is surface-only — offscreen it is an explicit no-op.
    // Note Auto at 30fps on a 60Hz output picks FIFO_RELAXED whenever the
    // surface advertises it (30 < 54 = 60*9/10); FIFO is only the fallback.
    void              setPresentPolicy(PresentModePolicy p) { m_present_policy = p; }
    void              setOutputRefreshHz(int hz) { m_output_refresh_hz = hz; }
    void              setTargetFps(int fps) { m_target_fps = fps; }
    PresentModePolicy presentPolicy() const { return m_present_policy; }
    int               outputRefreshHz() const { return m_output_refresh_hz; }
    int               targetFps() const { return m_target_fps; }

private:
    // Device holds a Swapchain by value and its own constructor is
    // user-provided, so these are default-initialized, never zeroed.  The
    // accessors are reachable before Create() runs (and stay reachable when
    // it fails), so give them values that mean "nothing has been picked yet"
    // rather than whatever was on the stack.
    vvk::SwapchainKHR            m_handle;
    VkSurfaceFormatKHR           m_format {};
    VkExtent2D                   m_extent {};
    VkPresentModeKHR             m_present_mode { VK_PRESENT_MODE_FIFO_KHR };
    std::vector<ImageParameters> m_images;
    std::vector<vvk::ImageView>  m_imageviews;

    PresentModePolicy m_present_policy { PresentModePolicy::Auto };
    int               m_target_fps { 30 };       // matches main.xml Fps default
    int               m_output_refresh_hz { 60 }; // safe assumption pre-monitor-query
};
} // namespace vulkan
} // namespace wallpaper
