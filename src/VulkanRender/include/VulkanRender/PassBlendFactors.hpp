#pragma once
#include <vulkan/vulkan.h>

#include "Type.hpp"

// BlendMode → Vulkan blend-state mapping.  Lives in its own header (rather
// than PassCommon.hpp) so the unit tests under src/Test can exercise it
// without pulling in the rest of the renderer.  The translucent / additive /
// opaque cases are documented inline.
namespace wallpaper
{
namespace vulkan
{
inline void SetBlend(BlendMode bm, VkPipelineColorBlendAttachmentState& state) {
    state.blendEnable  = true;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (bm) {
    case BlendMode::Disable: state.blendEnable = false; break;
    case BlendMode::Normal:
        // Overwrite: result = src.  The alpha-aware mapping
        // (SRC_ALPHA / ONE_MINUS_SRC_ALPHA) is the by-the-book
        // interpretation of WE's "normal" blend mode and pins a halo
        // case on Naruto Shippuden 2800255344, but darkens or shifts
        // the look of other wallpapers in ways that aren't worth the
        // single-scene fix — keep this as straight overwrite.
        state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case BlendMode::Translucent:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case BlendMode::Opaque:
        // Screen blend: result.rgb = src + dst * (1 - src).
        // Where src is black (outside mask): result = dst (background preserved).
        // Where src has content (inside mask): result ≈ src (content dominates).
        // Alpha: preserve destination alpha (keeps wallpaper opaque at alpha=1).
        state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case BlendMode::Translucent_PA:
        // Premultiplied alpha: shader output RGB is already scaled by alpha.
        // Using ONE for src avoids the double-multiplication that SRC_ALPHA causes.
        state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    }
}

inline void SetAttachmentLoadOp(BlendMode bm, VkAttachmentLoadOp& load_op) {
    switch (bm) {
    case BlendMode::Disable: load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
    // Normal uses LOAD to preserve prior pass content in uncovered areas
    // (required for 3D scenes where geometry doesn't fill the viewport)
    case BlendMode::Normal:
    case BlendMode::Additive:
    case BlendMode::Translucent:
    case BlendMode::Translucent_PA:
    case BlendMode::Opaque: load_op = VK_ATTACHMENT_LOAD_OP_LOAD; break;
    }
}
} // namespace vulkan
} // namespace wallpaper
