#pragma once

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

} // namespace scenebackend
