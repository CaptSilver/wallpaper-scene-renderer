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

// Does this scene carry any author script at all?  setupTextScripts() builds
// the QJSEngine and every proxy only when this is true, so a script kind left
// out of it is silently inert: nothing compiles, nothing logs, and the value it
// was supposed to animate sits at whatever the parser produced.
// constantshadervalues scripts were the kind left out — a scene whose only
// scripts are shader-value scripts (a hue cycle on an effect uniform, say)
// never built an engine and never moved.
inline bool sceneHasAuthorScripts(bool hasTextScripts, bool hasColorScripts,
                                  bool hasPropertyScripts, bool hasSoundLayerControls,
                                  bool hasShaderValueScripts) {
    return hasTextScripts || hasColorScripts || hasPropertyScripts || hasSoundLayerControls ||
           hasShaderValueScripts;
}

// Does the color loop have anything to evaluate?  Shader-value scripts have no
// timer of their own — evaluateColorScripts() runs both kinds, so the color
// loop is their only driver.  This one predicate answers the question at all
// three places that ask it (whether to create m_colorTimer, the loop body's own
// early return, and the chained call from the property tick); before, the timer
// gate named only the color states, so a scene with shader-value scripts but no
// color scripts compiled them and then never called them unless it happened to
// also run the property loop.
inline bool colorLoopHasWork(bool hasColorStates, bool hasShaderValueStates) {
    return hasColorStates || hasShaderValueStates;
}

// render-frame gate for the property loop (mirrors the gate baked
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

// audio-buffer refresh de-dup.  refreshAudioBuffers() is called from
// the property loop (now render-gated), the text loop (render-gated),
// and the color loop (33Hz); the analyzer only produces new spectrum data per
// processed render frame, so a refresh whose frame index matches the
// previously-refreshed one is redundant.  Returns true (do the refresh) iff
// this is the first refresh (frame 0) or the frame index advanced — collapsing
// all callers to one actual rebuild per drawn frame.
inline bool audioRefreshShouldRun(uint64_t curFrameIdx, uint64_t lastRefreshedFrameIdx) {
    return curFrameIdx == 0 || curFrameIdx != lastRefreshedFrameIdx;
}

} // namespace scenebackend
