#include <doctest.h>

#include "VulkanRender/CustomShaderPass.hpp"

#include <vulkan/vulkan.h>

using wallpaper::vulkan::DepthImageUsageFlags;

// GetOrCreateDepthImage composes the depth image's usage flags by calling
// DepthImageUsageFlags, so these cases pin the bits that function returns:
// which ones are unconditional, and which one the device's ability to sample
// D32 decides.
TEST_SUITE("DepthUsageFlags") {
    TEST_CASE("path A composes DEPTH_STENCIL_ATTACHMENT | TRANSFER_DST | SAMPLED") {
        VkImageUsageFlags usage = DepthImageUsageFlags(/*d32_sampleable=*/true);
        CHECK((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0u);
        CHECK((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0u);
        CHECK((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0u);
    }
    TEST_CASE("a device without sampled D32 support gets no SAMPLED_BIT on the depth "
              "attachment") {
        VkImageUsageFlags usage = DepthImageUsageFlags(/*d32_sampleable=*/false);
        CHECK((usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0u);
    }
}
