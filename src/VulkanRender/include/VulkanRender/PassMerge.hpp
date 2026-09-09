#pragma once
#include <algorithm>
#include <span>
#include <vector>

// Pure decision layer for recording several consecutive scene passes inside
// ONE Vulkan render pass instance instead of a begin/end cycle each.
//
// The renderer emits one render pass per scene node.  A scene whose layers
// all composite into _rt_default therefore pays a full-screen
// begin/store/transition — and under MSAA a full-screen resolve — per layer,
// and frame time tracks the pass count almost linearly.  Consecutive passes
// that target the same attachments can be recorded as consecutive draws in
// one render pass instead: same output, same pixels, one boundary.
//
// Header-only so the doctest tests exercise it without a Vulkan device.
namespace wallpaper::vulkan
{

// Where a pass sits in a run of passes sharing one render pass instance.
enum class PassMergeRole
{
    Standalone,  // opens and closes its own render pass (the old behaviour)
    GroupBegin,  // opens the render pass and leaves it open
    GroupMiddle, // draws into an already-open render pass
    GroupEnd,    // draws, then closes the render pass
};

inline bool PassOpensRenderPass(PassMergeRole r) {
    return r == PassMergeRole::Standalone || r == PassMergeRole::GroupBegin;
}

inline bool PassClosesRenderPass(PassMergeRole r) {
    return r == PassMergeRole::Standalone || r == PassMergeRole::GroupEnd;
}

// May a hidden pass drop its whole recording (the invisible-node fast path in
// CustomShaderPass::execute), or does it still owe the group a render pass
// boundary?  Vulkan gives no way to close a render pass another pass opened,
// so a hidden GroupBegin/GroupEnd still records begin/end and contributes
// only a missing draw.  It costs nothing extra: the boundary it records is
// the one the whole group shares.
inline bool HiddenPassDropsRecording(PassMergeRole r) {
    return r == PassMergeRole::Standalone || r == PassMergeRole::GroupMiddle;
}

// What ComputePassMergeRoles needs to know about one pass, in frame
// execution order.  Handles are opaque — the tests instantiate with an
// integer type, the renderer with VkImageView / VkImage.
template<typename H>
struct PassMergeInfo {
    // False for anything that is not a plain draw into its own attachments:
    // PrePass, FinPass, CopyPass, a pass that failed to prepare, or one that
    // may skip itself on re-execute (its early return would strand the
    // group's begin or end).  Such a pass is standalone and breaks any run.
    bool mergeable { false };

    // Framebuffer/render-pass identity.  Two passes may share a render pass
    // instance only if all of these agree: the leader's framebuffer is the
    // one that stays bound, and every member's pipeline must have been built
    // against a render pass compatible with the leader's.  Render pass
    // compatibility ignores load/store ops and layouts but not formats,
    // sample counts or attachment references — which these pin down.
    H        color_view {}; // colour attachment; the resolve target under MSAA
    H        msaa_view {};  // multisampled colour attachment, null without MSAA
    H        depth_view {};
    unsigned samples { 1 };
    unsigned width { 0 };
    unsigned height { 0 };

    // This pass's colour attachment load op is CLEAR.  Only the pass that
    // opens the render pass gets its load op honoured, so a clearing pass can
    // lead a group but never join one.
    bool clears_output { false };

    // The images the group writes (colour, MSAA colour, depth) and the images
    // this pass samples.  A pass that samples an attachment of the run it
    // would join needs the render pass boundary as its barrier.
    std::vector<H> attachment_images {};
    std::vector<H> sampled_images {};
};

namespace detail
{
template<typename H>
bool SharesAttachmentConfig(const PassMergeInfo<H>& a, const PassMergeInfo<H>& b) {
    return a.color_view == b.color_view && a.msaa_view == b.msaa_view &&
           a.depth_view == b.depth_view && a.samples == b.samples && a.width == b.width &&
           a.height == b.height;
}

template<typename H>
bool SamplesAnyOf(const PassMergeInfo<H>& pass, const std::vector<H>& images) {
    for (const H& sampled : pass.sampled_images) {
        if (sampled == H {}) continue;
        if (std::find(images.begin(), images.end(), sampled) != images.end()) return true;
    }
    return false;
}

// Can this pass take part in a merged group at all, as leader or member?
template<typename H>
bool CanJoinAnyGroup(const PassMergeInfo<H>& pass) {
    if (! pass.mergeable) return false;
    if (pass.color_view == H {}) return false;
    // Sampling your own attachment is a feedback loop the render pass
    // boundary currently papers over; leave those passes alone entirely.
    return ! SamplesAnyOf(pass, pass.attachment_images);
}
} // namespace detail

// May `cand`, which comes immediately after the run led by `leader`, be
// recorded inside the leader's render pass instance?
template<typename H>
bool CanFollowInGroup(const PassMergeInfo<H>& leader, const PassMergeInfo<H>& cand) {
    if (! detail::CanJoinAnyGroup(leader) || ! detail::CanJoinAnyGroup(cand)) return false;
    if (! detail::SharesAttachmentConfig(leader, cand)) return false;
    if (cand.clears_output) return false;
    return ! detail::SamplesAnyOf(cand, leader.attachment_images);
}

// Assigns each pass its role, greedily extending a run for as long as the
// next pass may follow the run's leader.  Order is never changed: only
// adjacent passes merge, so anything that reads the target — an effect chain
// sampling _rt_default, a copy, a bloom stage — sits between two runs and
// keeps the barrier it depends on.
template<typename H>
std::vector<PassMergeRole> ComputePassMergeRoles(std::span<const PassMergeInfo<H>> passes) {
    std::vector<PassMergeRole> roles(passes.size(), PassMergeRole::Standalone);

    std::size_t i = 0;
    while (i < passes.size()) {
        std::size_t end = i + 1;
        while (end < passes.size() && CanFollowInGroup(passes[i], passes[end])) ++end;

        if (end - i >= 2) {
            roles[i] = PassMergeRole::GroupBegin;
            for (std::size_t k = i + 1; k + 1 < end; ++k) roles[k] = PassMergeRole::GroupMiddle;
            roles[end - 1] = PassMergeRole::GroupEnd;
        }
        i = end;
    }
    return roles;
}

} // namespace wallpaper::vulkan
