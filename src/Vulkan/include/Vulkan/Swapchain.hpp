#pragma once
#include "Instance.hpp"
#include <span>

namespace wallpaper
{
namespace vulkan
{
struct ImageParameters;
struct VmaImageParameters;

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

private:
    vvk::SwapchainKHR            m_handle;
    VkSurfaceFormatKHR           m_format;
    VkExtent2D                   m_extent;
    VkPresentModeKHR             m_present_mode;
    std::vector<ImageParameters> m_images;
    std::vector<vvk::ImageView>  m_imageviews;
};
} // namespace vulkan
} // namespace wallpaper
