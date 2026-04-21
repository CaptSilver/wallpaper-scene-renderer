#pragma once
#include <cstddef>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Pure helpers for alias-aware pass caching + the invisible-node fast path
// in CustomShaderPass::execute.  Header-only so the doctest tests can
// exercise them without a Vulkan device.
namespace wallpaper::vulkan
{

// For a sequence of output image handles (one per pass in frame-execution
// order), returns a parallel vector: result[i] is true iff handle[i] is
// the LAST occurrence in the sequence (no later pass writes the same
// handle).  Entries where handle == H{} always get false.
//
// Used by VulkanRender::compileRenderGraph to decide which cacheable
// passes are safe to skip-on-re-execute — a pass's cached RT bytes only
// persist across frames if nothing later in the frame overwrites them.
template <typename H>
std::vector<bool> ComputeIsLastWriter(std::span<const H> outputs) {
    std::unordered_map<H, std::size_t> last_index;
    last_index.reserve(outputs.size());
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i] != H {}) last_index[outputs[i]] = i;
    }
    std::vector<bool> result(outputs.size(), false);
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        auto h = outputs[i];
        if (h != H {} && last_index[h] == i) result[i] = true;
    }
    return result;
}

// Invisible-node fast path guard.  A runtime-hidden pass can skip its
// whole execute (no render pass begin/end, no barriers, no descriptor
// pushes) only when doing so wouldn't strand a depth image in
// VK_IMAGE_LAYOUT_UNDEFINED — either the pass has no depth, or the
// depth image is absent, or some earlier visible pass this frame has
// already initialised it.
template <typename DepthHandle, typename InitedSet>
bool IsDepthSafeToSkip(bool has_depth, DepthHandle depth_image,
                       const InitedSet& already_inited_this_frame) {
    if (! has_depth) return true;
    if (depth_image == DepthHandle {}) return true;
    return already_inited_this_frame.count(depth_image) > 0;
}

} // namespace wallpaper::vulkan
