// Pin the Vulkan blend-factor mapping for each BlendMode enum case.
//
// Naruto Shippuden 2800255344 (effects/spin compose) used "blending":"normal"
// on a 2000x2000 RT whose content was a round sun with alpha=0 corners.  The
// previous Normal mapping (ONE/ZERO color factors) was an OVERWRITE blend and
// echoed the corner RGB through to _rt_default as a 1-pixel orange halo
// outside the round sun.  This suite locks the SrcAlpha/OneMinusSrcAlpha mapping
// in so future BlendMode tweaks have to acknowledge it.

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

    TEST_CASE("Normal uses alpha-aware factors so RGB at alpha=0 doesn't leak") {
        auto s = makeState();
        SetBlend(BlendMode::Normal, s);
        CHECK(s.blendEnable == VK_TRUE);
        CHECK(s.colorBlendOp == VK_BLEND_OP_ADD);
        CHECK(s.alphaBlendOp == VK_BLEND_OP_ADD);
        // Color: SrcAlpha / OneMinusSrcAlpha — opaque sources still write
        // src directly (1*src + 0*dst = src), but the round-on-square case
        // properly hides the alpha=0 corners.
        CHECK(s.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
        CHECK(s.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
        // Alpha factors stay overwrite — destination alpha matches what the
        // source shader wrote.  FinPass forces _rt_default alpha=1 anyway.
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
