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
template<typename H>
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

// Returns a parallel vector: result[i] is true iff pass i is the SOLE
// writer of outputs[i] (no other pass in the sequence — earlier OR later —
// writes the same handle).  Entries where outputs[i] == H{} always get false.
//
// Required invariant for pass-output caching.  "Last writer" is not enough:
// if an EARLIER pass also writes the same RT (e.g. multiple draws
// accumulating into _rt_default), that earlier pass will overwrite the
// cached bytes on the NEXT frame before the skipped pass's consumer reads
// them.  Only sole-writer passes can safely skip on re-execute.
template<typename H>
std::vector<bool> ComputeIsSoleWriter(std::span<const H> outputs) {
    std::unordered_map<H, std::size_t> count;
    count.reserve(outputs.size());
    for (const H& h : outputs) {
        if (h != H {}) ++count[h];
    }
    std::vector<bool> result(outputs.size(), false);
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        const H& h = outputs[i];
        if (h != H {} && count[h] == 1) result[i] = true;
    }
    return result;
}

// Full "safe to skip on re-exec" decision.  Pass i may skip iff:
//   (a) is_cacheable[i] — pass's own uniforms are stable (no g_Time /
//       g_PointerPosition / g_AudioSpectrum*);
//   (b) pass i is the sole writer of outputs[i] — no alias would
//       overwrite the cached bytes on a later frame;
//   (c) every RT-input in inputs[i] is either external/static (not in
//       outputs — e.g. bitmap textures) OR produced by another pass that
//       is itself safe-to-skip.  Ensures the pass's output stays
//       invariant across frames so the cached bytes remain correct.
//
// Computed by fixpoint: initial candidates = (a) ∧ (b); peel off any
// candidate that violates (c); repeat until stable.  Guaranteed to
// terminate because each iteration strictly shrinks the candidate set.
template<typename H>
std::vector<bool> ComputeIsSafeToSkip(std::span<const H>              outputs,
                                      std::span<const std::vector<H>> inputs,
                                      const std::vector<bool>&        is_cacheable) {
    const std::size_t N = outputs.size();

    auto sole = ComputeIsSoleWriter<H>(outputs);

    std::vector<bool> safe(N, false);
    for (std::size_t i = 0; i < N; ++i) {
        if (i < is_cacheable.size() && is_cacheable[i] && sole[i]) safe[i] = true;
    }

    // Map H → writer-pass-index.  If a handle has multiple writers it's
    // already non-sole so already unsafe; which one we record doesn't matter
    // because looking it up and finding !safe reaches the correct verdict.
    std::unordered_map<H, std::size_t> writer_of;
    writer_of.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        if (outputs[i] != H {}) writer_of[outputs[i]] = i;
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < N; ++i) {
            if (! safe[i]) continue;
            if (i >= inputs.size()) continue;
            for (const H& in : inputs[i]) {
                if (in == H {}) continue;
                auto it = writer_of.find(in);
                if (it == writer_of.end()) continue; // external / static
                if (! safe[it->second]) {
                    safe[i] = false;
                    changed = true;
                    break;
                }
            }
        }
    }

    return safe;
}

// Invisible-node fast path guard.  A runtime-hidden pass can skip its
// whole execute (no render pass begin/end, no barriers, no descriptor
// pushes) only when doing so wouldn't strand a depth image in
// VK_IMAGE_LAYOUT_UNDEFINED — either the pass has no depth, or the
// depth image is absent, or some earlier visible pass this frame has
// already initialised it.
template<typename DepthHandle, typename InitedSet>
bool IsDepthSafeToSkip(bool has_depth, DepthHandle depth_image,
                       const InitedSet& already_inited_this_frame) {
    if (! has_depth) return true;
    if (depth_image == DepthHandle {}) return true;
    return already_inited_this_frame.count(depth_image) > 0;
}

} // namespace wallpaper::vulkan
