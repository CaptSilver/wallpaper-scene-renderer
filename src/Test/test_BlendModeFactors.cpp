// Pin the Vulkan blend-factor mapping for each BlendMode enum case.

#include <doctest.h>

#include "Type.hpp"
#include "VulkanRender/PassBlendFactors.hpp"

using wallpaper::BlendMode;
using wallpaper::vulkan::SetBlend;

namespace {
VkPipelineColorBlendAttachmentState makeState() {
    VkPipelineColorBlendAttachmentState s {};
    return s;
}
} // namespace

TEST_SUITE("BlendMode → Vulkan factors") {
    TEST_CASE("Disable turns blending off") {
        auto s = makeState();
        SetBlend(BlendMode::Disable, s);
        CHECK(s.blendEnable == VK_FALSE);
    }

    TEST_CASE("Normal is straight overwrite (src replaces dst on both color and alpha)") {
        auto s = makeState();
        SetBlend(BlendMode::Normal, s);
        CHECK(s.blendEnable == VK_TRUE);
        CHECK(s.colorBlendOp == VK_BLEND_OP_ADD);
        CHECK(s.alphaBlendOp == VK_BLEND_OP_ADD);
        CHECK(s.srcColorBlendFactor == VK_BLEND_FACTOR_ONE);
        CHECK(s.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO);
        CHECK(s.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
        CHECK(s.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO);
    }

    TEST_CASE("Translucent uses standard alpha blend with reverse-multiplied dst alpha") {
        auto s = makeState();
        SetBlend(BlendMode::Translucent, s);
        CHECK(s.blendEnable == VK_TRUE);
        CHECK(s.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        CHECK(s.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        CHECK(s.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
        CHECK(s.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    }

    TEST_CASE("Additive accumulates color via SrcAlpha + ONE") {
        auto s = makeState();
        SetBlend(BlendMode::Additive, s);
        CHECK(s.blendEnable == VK_TRUE);
        CHECK(s.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        CHECK(s.dstColorBlendFactor == VK_BLEND_FACTOR_ONE);
        CHECK(s.srcAlphaBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        CHECK(s.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
    }

    TEST_CASE("Opaque is screen-blend (ONE * src + (1-src) * dst)") {
        auto s = makeState();
        SetBlend(BlendMode::Opaque, s);
        CHECK(s.blendEnable == VK_TRUE);
        CHECK(s.srcColorBlendFactor == VK_BLEND_FACTOR_ONE);
        CHECK(s.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR);
        CHECK(s.srcAlphaBlendFactor == VK_BLEND_FACTOR_ZERO);
        CHECK(s.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
    }

    TEST_CASE("Translucent_PA expects shader to premultiply src.rgb by src.a") {
        auto s = makeState();
        SetBlend(BlendMode::Translucent_PA, s);
        CHECK(s.blendEnable == VK_TRUE);
        CHECK(s.srcColorBlendFactor == VK_BLEND_FACTOR_ONE);
        CHECK(s.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        CHECK(s.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
        CHECK(s.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    }
}
