#pragma once
#include <cstddef>
#include <span>
#include <utility>
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

// Should a pass ComputeIsSafeToSkip already cleared for caching actually
// have canCache() set?  Folds the two carve-outs the cache-gating loop in
// VulkanRender::compileRenderGraph applies on top of the alias verdict:
//   - _rt_default is never cached -- PrePass clears it every frame, so a
//     cached pass there would leave a stale clear-color on screen.
//   - a terminal-hideable pass is never cached, even on a private offscreen
//     RT: which pipeline it draws (real vs. passthrough) depends on runtime
//     visibility, so a cached frame would freeze whichever one happened to
//     be current through the next toggle.
//     This backs up CacheableForSafetyAnalysis, which already keeps hideable
//     passes out of the safety analysis itself.
inline bool EligibleForOutputCache(bool is_rt_default, bool is_hideable, bool safe_to_skip) {
    if (is_rt_default) return false;
    if (is_hideable) return false;
    return safe_to_skip;
}

// A cached pass keeps its bytes only while every image it samples is untouched.
// ComputeIsSafeToSkip treats a cacheable producer as invariant, but a cached
// producer still re-runs whenever its own uniforms go dirty (a script drives
// its alpha, a user property changes) and that redraw never reaches the
// consumers that sampled the old bytes.  rewritten_this_frame holds every
// output image a pass actually drew so far this frame; a cached pass whose
// input is in it must redraw, and then adds its own output so the redraw
// carries on down the chain.  Null handles (unbound slots) never match.
template<typename H, typename Set>
bool CachedPassMustRedraw(std::span<const H> inputs, const Set& rewritten_this_frame) {
    for (const H& in : inputs) {
        if (in != H {} && rewritten_this_frame.count(in) > 0) return true;
    }
    return false;
}

// Per pass, the inputs that some pass at the same or a later position writes.
// The render graph orders a reader of a render target ahead of a later writer
// of it (the reader gets the previous frame's content, same VkImage), so a
// writer that re-runs in frame F reaches such a reader only in frame F + 1.
// Forward inputs are covered by this frame's set; these need last frame's.
template<typename H>
std::vector<std::vector<H>> ClassifyBackwardInputs(std::span<const H>              outputs,
                                                   std::span<const std::vector<H>> inputs) {
    std::unordered_map<H, std::size_t> last_writer;
    last_writer.reserve(outputs.size());
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i] != H {}) last_writer[outputs[i]] = i;
    }
    std::vector<std::vector<H>> result(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        for (const H& in : inputs[i]) {
            if (in == H {}) continue;
            auto it = last_writer.find(in);
            if (it != last_writer.end() && it->second >= i) result[i].push_back(in);
        }
    }
    return result;
}

struct PassCacheDecision {
    bool invalidate { false }; // drop the cached bytes; the pass must draw
    bool skip { false };       // keep them; execute() may return now
};

// The per-pass cache step at the top of CustomShaderPass::execute.  A cached
// pass is dropped when its own uniforms went dirty, when its node's visibility
// is not what it was cached with (a hide script never dirties the material, and
// the hidden path is decided after the cache early-out), or when an input was
// redrawn: `inputs` against this frame's set, `backward_inputs` (see
// ClassifyBackwardInputs) against last frame's.  Either set may be null when
// the pass was never wired.  Skipping also needs the pass to be standalone,
// since a merged pass re-records to keep its render pass group whole.
template<typename H, typename Set>
PassCacheDecision DecidePassCache(bool can_cache, bool is_cached, bool own_material_dirty,
                                  bool visible_now, bool visible_when_cached,
                                  std::span<const H> inputs, std::span<const H> backward_inputs,
                                  const Set* rewritten_this_frame, const Set* rewritten_last_frame,
                                  bool standalone) {
    PassCacheDecision d;
    if (! can_cache || ! is_cached) return d;
    d.invalidate =
        own_material_dirty || visible_now != visible_when_cached ||
        (rewritten_this_frame && CachedPassMustRedraw<H>(inputs, *rewritten_this_frame)) ||
        (rewritten_last_frame && CachedPassMustRedraw<H>(backward_inputs, *rewritten_last_frame));
    d.skip = ! d.invalidate && standalone;
    return d;
}

// Called once a pass has recorded its draw: consumers sampling `output` must
// not trust their own cache for the rest of the frame.  Null output or an
// unwired set records nothing.
template<typename H, typename Set>
void RecordPassDrawn(Set* rewritten_this_frame, H output) {
    if (rewritten_this_frame && output != H {}) rewritten_this_frame->insert(output);
}

// Start of a frame: what was drawn becomes "last frame", and this frame starts
// empty.  Swap + clear keeps the bucket arrays; each insert still allocates a
// node, as m_depth_inited_frame already does.
template<typename Set>
void AdvanceRewrittenFrame(Set& this_frame, Set& last_frame) {
    std::swap(this_frame, last_frame);
    this_frame.clear();
}

// The cacheability a pass contributes to ComputeIsSafeToSkip. A hideable pass
// draws its real material or a passthrough depending on runtime visibility, so
// it is never cacheable -- and that has to be known inside the analysis, not
// applied afterwards, or a static pass reading its output is wrongly judged
// safe to skip and freezes across the next toggle.
inline bool CacheableForSafetyAnalysis(bool is_cacheable, bool is_hideable) {
    return is_cacheable && ! is_hideable;
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

// recordDepthInit()'s own guard: should THIS call issue the depth image's
// clear-and-transition, or has an earlier pass this frame already covered
// it?  The mirror of IsDepthSafeToSkip, plus the precondition that the
// tracker is actually wired -- a null tracker means compileRenderGraph
// never called setDepthInitedFrameSet on this pass (a construction-order
// bug the caller asserts on in debug builds); a release build just
// declines to record rather than dereferencing null.
template<typename DepthHandle, typename InitedSet>
bool ShouldRecordDepthInit(bool has_depth, DepthHandle depth_image,
                           const InitedSet* already_inited_this_frame) {
    if (already_inited_this_frame == nullptr) return false;
    return ! IsDepthSafeToSkip(has_depth, depth_image, *already_inited_this_frame);
}

} // namespace wallpaper::vulkan
