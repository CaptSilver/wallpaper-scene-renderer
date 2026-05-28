#pragma once
#include "Instance.hpp"
#include <span>

namespace wallpaper
{
namespace vulkan
{
struct ImageParameters;
struct VmaImageParameters;

// Policy by which the swapchain picks among supported present modes.
// Auto is the default and selects based on target_fps vs output_refresh_hz.
// The four explicit modes are fall-backs the user can pin from the
// SettingPage ComboBox.  Each falls back to FIFO when the preferred mode
// is not advertised by the surface — FIFO is guaranteed by the Vulkan spec.
enum class PresentModePolicy
{
    Auto        = 0, // pick based on target_fps vs output_refresh_hz ratio
    Fifo        = 1, // strict vsync — current default before this commit
    FifoRelaxed = 2, // vsync but allow late-frame catch-up (sub-refresh fps smoothing)
    Mailbox     = 3, // unthrottled, drop frames (low-latency, may waste GPU)
    Immediate   = 4, // no vsync at all (rare; mostly benchmarking)
};

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

    // User-configurable inputs to pickPresentMode().  Take effect at the
    // next Create() or Recreate(); call mut_swapchain() through Device and
    // force a recreate via VulkanRender::setSwapchainPresentPolicy to apply
    // mid-session.  Defaults preserve today's behaviour: Auto with 30fps on
    // a 60Hz output keeps FIFO.
    void              setPresentPolicy(PresentModePolicy p) { m_present_policy = p; }
    void              setOutputRefreshHz(int hz) { m_output_refresh_hz = hz; }
    void              setTargetFps(int fps) { m_target_fps = fps; }
    PresentModePolicy presentPolicy() const { return m_present_policy; }
    int               outputRefreshHz() const { return m_output_refresh_hz; }
    int               targetFps() const { return m_target_fps; }

private:
    vvk::SwapchainKHR            m_handle;
    VkSurfaceFormatKHR           m_format;
    VkExtent2D                   m_extent;
    VkPresentModeKHR             m_present_mode;
    std::vector<ImageParameters> m_images;
    std::vector<vvk::ImageView>  m_imageviews;

    PresentModePolicy m_present_policy { PresentModePolicy::Auto };
    int               m_target_fps { 30 };       // matches main.xml Fps default
    int               m_output_refresh_hz { 60 }; // safe assumption pre-monitor-query
};
} // namespace vulkan
} // namespace wallpaper
