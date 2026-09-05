#include <doctest.h>
#include <vulkan/vulkan.h>

#include "Vulkan/SwapchainPolicy.hpp"

using namespace wallpaper::vulkan;

namespace
{
VkFormatProperties tilingFeatures(VkFormatFeatureFlags optimal, VkFormatFeatureFlags linear = 0) {
    VkFormatProperties p {};
    p.optimalTilingFeatures = optimal;
    p.linearTilingFeatures  = linear;
    return p;
}

constexpr VkFormatFeatureFlags kAllRequired = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

VkFormatProperties allExcept(VkFormatFeatureFlagBits bit) {
    return tilingFeatures(kAllRequired & ~static_cast<VkFormatFeatureFlags>(bit));
}
} // namespace

TEST_SUITE("Exported offscreen swapchain format") {
    TEST_CASE("HDR output asks for 16-bit float, everything else for 8-bit") {
        CHECK(exSwapchainFormat(true) == VK_FORMAT_R16G16B16A16_SFLOAT);
        CHECK(exSwapchainFormat(false) == VK_FORMAT_R8G8B8A8_UNORM);
    }

    // Turning HDR on tears the renderer down and builds it again.  If the
    // 16-bit format does not come back the wallpaper stays blank until the
    // user toggles the setting a second time, so 8-bit is worth taking.
    TEST_CASE("the 16-bit format can fall back to 8-bit") {
        auto c = exSwapchainFormatCandidates(VK_FORMAT_R16G16B16A16_SFLOAT);
        REQUIRE(c.size() == 2u);
        CHECK(c[0] == VK_FORMAT_R16G16B16A16_SFLOAT);
        CHECK(c[1] == VK_FORMAT_R8G8B8A8_UNORM);
    }

    TEST_CASE("8-bit is already the fallback and has none of its own") {
        auto c = exSwapchainFormatCandidates(VK_FORMAT_R8G8B8A8_UNORM);
        REQUIRE(c.size() == 1u);
        CHECK(c[0] == VK_FORMAT_R8G8B8A8_UNORM);
    }

    // A caller naming something else knows what it wants; substituting a
    // format it did not ask for would be worse than failing.
    TEST_CASE("any other format is taken as given") {
        auto c = exSwapchainFormatCandidates(VK_FORMAT_B8G8R8A8_UNORM);
        REQUIRE(c.size() == 1u);
        CHECK(c[0] == VK_FORMAT_B8G8R8A8_UNORM);
    }
}

// The exported images are rendered into, sampled back for the final
// composite, and blitted to, so all three features have to be there for
// whichever tiling the image is actually created with.
TEST_SUITE("Exported offscreen swapchain format support") {
    TEST_CASE("all three features present is usable") {
        CHECK(exSwapchainFormatUsable(tilingFeatures(kAllRequired), VK_IMAGE_TILING_OPTIMAL));
    }

    TEST_CASE("missing colour attachment is not usable") {
        auto p = allExcept(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        CHECK_FALSE(exSwapchainFormatUsable(p, VK_IMAGE_TILING_OPTIMAL));
    }

    TEST_CASE("missing sampled image is not usable") {
        auto p = allExcept(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        CHECK_FALSE(exSwapchainFormatUsable(p, VK_IMAGE_TILING_OPTIMAL));
    }

    TEST_CASE("missing transfer destination is not usable") {
        auto p = allExcept(VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
        CHECK_FALSE(exSwapchainFormatUsable(p, VK_IMAGE_TILING_OPTIMAL));
    }

    TEST_CASE("unsupported reads nothing at all") {
        CHECK_FALSE(exSwapchainFormatUsable(tilingFeatures(0), VK_IMAGE_TILING_OPTIMAL));
    }

    // Linear tiling is the fallback path for hosts that cannot import a
    // tiled dma-buf; it has its own feature set and drivers routinely
    // support far less there.
    TEST_CASE("linear tiling is judged on the linear feature set") {
        auto p = tilingFeatures(/*optimal=*/kAllRequired, /*linear=*/0);
        CHECK(exSwapchainFormatUsable(p, VK_IMAGE_TILING_OPTIMAL));
        CHECK_FALSE(exSwapchainFormatUsable(p, VK_IMAGE_TILING_LINEAR));
    }

    TEST_CASE("optimal tiling is judged on the optimal feature set") {
        auto p = tilingFeatures(/*optimal=*/0, /*linear=*/kAllRequired);
        CHECK_FALSE(exSwapchainFormatUsable(p, VK_IMAGE_TILING_OPTIMAL));
        CHECK(exSwapchainFormatUsable(p, VK_IMAGE_TILING_LINEAR));
    }
}
