#pragma once

#include <cstdint>

namespace scenebackend
{

// Gate for the SceneScript property + color evaluation loops (F19).
//
// The two GUI-thread timers that drive author scripts — m_propertyTimer
// (~125Hz / 8ms) and m_colorTimer (~30Hz / 33ms) — used to keep evaluating
// every wallpaper's scripts even after SceneObject::pause() paused the render
// thread (on TTY-switch / suspend / battery-pause / occlusion).  Unlike the
// text loop, which self-throttles on the render-frame index and idles when no
// new frame is produced, the property/color loops had only an empty-state
// early-return, so a paused wallpaper still burned a full 125Hz JS evaluation
// loop on the GUI thread for zero visible benefit — defeating the point of
// pausing on laptops.
//
// This predicate is the single source of truth for "should the property/color
// loop do work this tick".  Splitting it out (mirroring SceneAspect.h) lets it
// be unit-tested without constructing the Vulkan-backed SceneObject:
//   * hasStates  — the loop has at least one script/sound/listener to evaluate
//                  (the existing empty-state early-return).
//   * paused     — SceneObject::pause() was called and play() has not resumed.
// The loop runs iff there is something to run AND we are not paused.
inline bool scriptLoopShouldRun(bool hasStates, bool paused) { return hasStates && ! paused; }

// Spec 07 — render-frame gate for the property loop (mirrors the gate baked
// into evaluateTextScripts).  The property timer polls at ~125Hz, but unless
// this wallpaper opted into sub-frame physics stepping the script output is
// sampled only when the render thread draws a frame — so evaluating faster
// than the render rate is pure wasted CPU.  Returns true (do the work) iff:
//   * highRate    — the wallpaper opted into >render-rate stepping (3body), OR
//   * lastFrameIdx == 0 — the very first (seed) eval, which the ctor runs to
//                  populate shared.* before text/color scripts; frameIdx can
//                  be 0 before the render thread starts, so this branch keeps
//                  the seed eval ungated, OR
//   * curFrameIdx != lastFrameIdx — the render thread produced a new frame
//                  since the previous eval.
// engine.frametime is wall-clock (ComputeTickFrametime), so the longer
// inter-eval delta is reported correctly and `x += v*frametime` integrators
// are unaffected — exactly the property the text loop relies on.
inline bool propertyTickShouldEval(bool highRate, uint64_t curFrameIdx, uint64_t lastFrameIdx) {
    return highRate || lastFrameIdx == 0 || curFrameIdx != lastFrameIdx;
}

// Spec 08 — audio-buffer refresh de-dup.  refreshAudioBuffers() is called from
// the property loop (now Spec-07 render-gated), the text loop (render-gated),
// and the color loop (33Hz); the analyzer only produces new spectrum data per
// processed render frame, so a refresh whose frame index matches the
// previously-refreshed one is redundant.  Returns true (do the refresh) iff
// this is the first refresh (frame 0) or the frame index advanced — collapsing
// all callers to one actual rebuild per drawn frame.
inline bool audioRefreshShouldRun(uint64_t curFrameIdx, uint64_t lastRefreshedFrameIdx) {
    return curFrameIdx == 0 || curFrameIdx != lastRefreshedFrameIdx;
}

} // namespace scenebackend
