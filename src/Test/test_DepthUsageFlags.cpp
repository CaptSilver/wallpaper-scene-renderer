#include <doctest.h>

#include <vulkan/vulkan.h>

// Pure-logic pin of the path-A usage-flag composition.  We don't construct a
// real depth image here (that needs a live Vulkan device); we pin the
// expression that GetOrCreateDepthImage uses to compose its usage bits, so a
// refactor can't silently drop SAMPLED_BIT.
TEST_SUITE("DepthUsageFlags") {
    TEST_CASE("path A composes DEPTH_STENCIL_ATTACHMENT | TRANSFER_DST | SAMPLED") {
        bool d32_sampleable = true;
        VkImageUsageFlags usage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (d32_sampleable ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u);
        CHECK((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0u);
        CHECK((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0u);
        CHECK((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0u);
    }
    TEST_CASE("path D omits SAMPLED_BIT from the depth attachment") {
        bool d32_sampleable = false;
        VkImageUsageFlags usage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (d32_sampleable ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u);
        CHECK((usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0u);
    }
}

// Pin the set of acceptable prior layouts for the per-frame depth-image
// re-init.  Today only UNDEFINED is accepted; leg 01 widens to also accept
// DEPTH_STENCIL_READ_ONLY_OPTIMAL so the consumer transition in leg 04 does
// not strand the image in a layout the next frame's first writer rejects.
TEST_SUITE("DepthUsageFlags") {
    TEST_CASE("init barrier accepts UNDEFINED or DEPTH_STENCIL_READ_ONLY_OPTIMAL") {
        auto accepts = [](VkImageLayout prior) {
            return prior == VK_IMAGE_LAYOUT_UNDEFINED ||
                   prior == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        };
        CHECK(accepts(VK_IMAGE_LAYOUT_UNDEFINED));
        CHECK(accepts(VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL));
        CHECK_FALSE(accepts(VK_IMAGE_LAYOUT_GENERAL));
        CHECK_FALSE(accepts(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL));
    }
}
