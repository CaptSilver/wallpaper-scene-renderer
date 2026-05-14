// Pin the Vulkan blend-factor mapping for each BlendMode enum case.

#include <doctest.h>

#include "Type.hpp"
#include "VulkanRender/PassBlendFactors.hpp"

#include <array>

using wallpaper::BlendMode;
using wallpaper::vulkan::SetAttachmentLoadOp;
using wallpaper::vulkan::SetBlend;

namespace {
VkPipelineColorBlendAttachmentState makeState() {
    VkPipelineColorBlendAttachmentState s {};
    return s;
}

// Resolve a Vulkan blend factor to its scalar coefficient given (src, dst)
// RGBA in [0,1].  Covers the subset of factors used by our BlendMode mapping.
float factorRGB(VkBlendFactor f, const std::array<float, 4>& src,
                const std::array<float, 4>& dst, int channel) {
    switch (f) {
    case VK_BLEND_FACTOR_ZERO: return 0.0f;
    case VK_BLEND_FACTOR_ONE: return 1.0f;
    case VK_BLEND_FACTOR_SRC_ALPHA: return src[3];
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return 1.0f - src[3];
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return 1.0f - src[channel];
    default: return 0.0f;
    }
}

// Simulate one Vulkan blend op against the configured state.  Returns the
// resulting (r,g,b,a) in [0,1].  Asserts on unsupported blend ops.
std::array<float, 4> applyBlend(const VkPipelineColorBlendAttachmentState& s,
                                 const std::array<float, 4>&               src,
                                 const std::array<float, 4>&               dst) {
    REQUIRE(s.colorBlendOp == VK_BLEND_OP_ADD);
    REQUIRE(s.alphaBlendOp == VK_BLEND_OP_ADD);
    std::array<float, 4> out {};
    for (int c = 0; c < 3; ++c) {
        float fs = factorRGB(s.srcColorBlendFactor, src, dst, c);
        float fd = factorRGB(s.dstColorBlendFactor, src, dst, c);
        out[c]   = fs * src[c] + fd * dst[c];
    }
    float fsa = factorRGB(s.srcAlphaBlendFactor, src, dst, 3);
    float fda = factorRGB(s.dstAlphaBlendFactor, src, dst, 3);
    out[3]    = fsa * src[3] + fda * dst[3];
    return out;
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

// SetAttachmentLoadOp was unused until the Naruto 2800255344 halo fix surfaced
// the need to gate base-material-into-pingpong on CLEAR semantics.  Pin its
// current contract here so callers can rely on it.
TEST_SUITE("BlendMode → VkAttachmentLoadOp") {
    TEST_CASE("Disable maps to DONT_CARE") {
        VkAttachmentLoadOp op = VK_ATTACHMENT_LOAD_OP_LOAD;
        SetAttachmentLoadOp(BlendMode::Disable, op);
        CHECK(op == VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    }
    TEST_CASE("Translucent maps to LOAD (composes with prior content)") {
        VkAttachmentLoadOp op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        SetAttachmentLoadOp(BlendMode::Translucent, op);
        CHECK(op == VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    TEST_CASE("Normal maps to LOAD (composes with prior content)") {
        VkAttachmentLoadOp op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        SetAttachmentLoadOp(BlendMode::Normal, op);
        CHECK(op == VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    TEST_CASE("Additive / Translucent_PA / Opaque map to LOAD") {
        VkAttachmentLoadOp op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        SetAttachmentLoadOp(BlendMode::Additive, op);
        CHECK(op == VK_ATTACHMENT_LOAD_OP_LOAD);
        op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        SetAttachmentLoadOp(BlendMode::Translucent_PA, op);
        CHECK(op == VK_ATTACHMENT_LOAD_OP_LOAD);
        op = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        SetAttachmentLoadOp(BlendMode::Opaque, op);
        CHECK(op == VK_ATTACHMENT_LOAD_OP_LOAD);
    }
}

// Property tests for the actual blend math given the factors above.  Captures
// the invariant the Naruto 2800255344 halo fix relies on: when the base
// material of an effect chain renders translucent into a cleared pingpong, an
// alpha=0 pixel with arbitrary RGB resolves to (0,0,0,0) — i.e. the pingpong
// becomes premultiplied, not overwritten.  Without this, bilinear filtering at
// the alpha boundary later in the chain reintroduces source-RGB-tinted halos.
TEST_SUITE("Blend math — pingpong initialization") {
    TEST_CASE("Translucent over cleared dst at src.a=0 yields (0,0,0,0)") {
        auto s = makeState();
        SetBlend(BlendMode::Translucent, s);
        // src = orange RGB but fully transparent (the Naruto eye.tex corner pixel)
        std::array<float, 4> src { 1.0f, 0.18f, 0.0f, 0.0f };
        std::array<float, 4> dst { 0.0f, 0.0f, 0.0f, 0.0f };
        auto                 out = applyBlend(s, src, dst);
        CHECK(out[0] == doctest::Approx(0.0f));
        CHECK(out[1] == doctest::Approx(0.0f));
        CHECK(out[2] == doctest::Approx(0.0f));
        CHECK(out[3] == doctest::Approx(0.0f));
    }
    TEST_CASE("Translucent over cleared dst at src.a=0.5 yields premultiplied (0.5*rgb, 0.5)") {
        auto s = makeState();
        SetBlend(BlendMode::Translucent, s);
        // Bilinear boundary pixel: half-coverage of the texture's orange edge.
        std::array<float, 4> src { 1.0f, 0.18f, 0.0f, 0.5f };
        std::array<float, 4> dst { 0.0f, 0.0f, 0.0f, 0.0f };
        auto                 out = applyBlend(s, src, dst);
        CHECK(out[0] == doctest::Approx(0.5f));
        CHECK(out[1] == doctest::Approx(0.09f));
        CHECK(out[2] == doctest::Approx(0.0f));
        CHECK(out[3] == doctest::Approx(0.5f));
    }
    TEST_CASE("Translucent over cleared dst at src.a=1 yields src.rgb unchanged") {
        auto s = makeState();
        SetBlend(BlendMode::Translucent, s);
        std::array<float, 4> src { 1.0f, 0.18f, 0.0f, 1.0f };
        std::array<float, 4> dst { 0.0f, 0.0f, 0.0f, 0.0f };
        auto                 out = applyBlend(s, src, dst);
        CHECK(out[0] == doctest::Approx(1.0f));
        CHECK(out[1] == doctest::Approx(0.18f));
        CHECK(out[2] == doctest::Approx(0.0f));
        CHECK(out[3] == doctest::Approx(1.0f));
    }
    TEST_CASE("Normal/overwrite into cleared dst leaks src.rgb at src.a=0 — the OLD bug") {
        // Pins the symptom: Normal blend writes src.rgb regardless of src.a,
        // so an alpha=0 orange-RGB texel poisons the pingpong with orange.
        // ParseImageObj's hasEffect branch used to force this mode; the fix
        // is to let the base material keep its declared blend (translucent).
        auto s = makeState();
        SetBlend(BlendMode::Normal, s);
        std::array<float, 4> src { 1.0f, 0.18f, 0.0f, 0.0f };
        std::array<float, 4> dst { 0.0f, 0.0f, 0.0f, 0.0f };
        auto                 out = applyBlend(s, src, dst);
        CHECK(out[0] == doctest::Approx(1.0f));   // <-- bug: orange survives
        CHECK(out[3] == doctest::Approx(0.0f));
    }
}
