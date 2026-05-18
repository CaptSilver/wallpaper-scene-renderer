#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "Core/Literals.hpp"

namespace wallpaper
{

// Named keyframe track targeting a scalar node property (currently "alpha").
// Parsed from WE scene.json layer properties that embed:
//   "<prop>": { "animation": { "c0":[{frame,value,…}], "options":{…} }, "value": <init> }
// Exposed to SceneScript via thisScene.getLayer(name).getAnimation(animName).
struct PropertyAnimKeyframe {
    float frame { 0.0f };
    float value { 0.0f };
    // Bezier handle data is present in WE's keyframes (`front`, `back`) but we
    // only implement linear interpolation today.  Faithful matching of WE's
    // tangent-based easing is a future refinement.
};

enum class PropertyAnimMode
{
    Loop,   // restart from frame 0
    Mirror, // bounce 0→length→0→length…
    Single  // play once, then hold last frame
};

inline PropertyAnimMode ParsePropertyAnimMode(std::string_view s) {
    if (s == "mirror") return PropertyAnimMode::Mirror;
    if (s == "single") return PropertyAnimMode::Single;
    return PropertyAnimMode::Loop;
}

struct PropertyAnimation {
    std::string name;     // animation name (from options.name) — script lookup key
    std::string property; // target property — scalars ("alpha"), vec3 components
                          // ("origin.x", "origin.y", "origin.z", "scale.*",
                          // "angles.*") for multi-channel WE tracks that store
                          // c0/c1/c2 keyframe arrays on the same animation.

    PropertyAnimMode mode { PropertyAnimMode::Loop };
    float            fps { 30.0f };
    float            length { 0.0f }; // in frames
    bool             startPaused { false };

    // For vec3 component anims: when true the keyframe values are *deltas*
    // applied on top of `initialValue` (the JSON `value` field for that
    // component).  When false the keyframe values replace the component
    // outright.  Maps WE's per-vec3-animation `relative` flag; the Rella
    // whale (3363252053 id=173) authors a relative origin animation that
    // drifts the school of fish around its base origin.
    bool relative { false };

    // WE's animation.options.wraploop flag.  When true (and mode=Loop), the
    // segment from the LAST authored keyframe back to length-frames is
    // interpolated linearly from kf.back().value to kf.front().value, so the
    // cycle closes smoothly instead of holding the last value and snapping
    // back at the period boundary.  Rella whale (3363252053) authors c1 with
    // keys at 0/60 over length=120 plus wraploop:true — without this the
    // whale dives down (frame 0→60) then *holds* the down position for
    // another 2s and snaps up — visibly jumpy.
    bool wraploop { false };

    std::vector<PropertyAnimKeyframe> keyframes;

    // Fallback value when no keyframes or playback hasn't started.  For
    // vec3-component anims this holds the corresponding axis of the JSON
    // `value` string so `relative` mode can add deltas onto the base.
    float initialValue { 1.0f };

    // Runtime state (mutated on the render thread only).
    bool   playing { false };
    double time { 0.0 };
};

// Evaluate the animation curve at a given playback time (seconds).
// Pure function — safe to exercise from tests.
//
// Implementation notes (mutation hardening):
// - `std::lower_bound` for keyframe lookup eliminates the linear-scan
//   boundary mutants (`i + 1 < kf.size()` equivalents) that survive because
//   the front/back guards trap exactly the frames that would otherwise hit
//   the loop's boundary indices.
// - Mirror mode uses strict `<` (not `<=`) on the period midpoint so the
//   `u == period` case has a single canonical path (returns u==period
//   regardless, but the comparison is observable to == vs < mutators).
inline float EvaluatePropertyAnimation(const PropertyAnimation& anim, double time) {
    const auto& kf = anim.keyframes;
    if (kf.empty()) return anim.initialValue;
    if (kf.size() == 1) return kf[0].value;

    if (anim.fps <= 0.0f) return kf[0].value;
    const double period = (double)anim.length / (double)anim.fps;
    if (period <= 0.0) return kf[0].value;

    double t = time;
    switch (anim.mode) {
    case PropertyAnimMode::Loop: {
        t = std::fmod(time, period);
        if (t < 0.0) t += period;
        break;
    }
    case PropertyAnimMode::Mirror: {
        const double T = period * 2.0;
        double u = std::fmod(time, T);
        if (u < 0.0) u += T;
        t = (u < period) ? u : (T - u);
        break;
    }
    case PropertyAnimMode::Single: t = std::clamp(time, 0.0, period); break;
    }

    const float frame = (float)(t * (double)anim.fps);
    if (frame <= kf.front().frame) return kf.front().value;
    // Wraploop (Loop mode only): instead of holding the last keyframe value
    // until the cycle restarts (which produces a visible snap), interpolate
    // from kf.back() back to kf.front() across the segment
    // [kf.back().frame, length].  Mirror/Single ignore wraploop — Mirror
    // already closes its cycle, Single intentionally holds at the end.
    if (anim.wraploop && anim.mode == PropertyAnimMode::Loop && anim.length > kf.back().frame &&
        frame >= kf.back().frame) {
        const float span = anim.length - kf.back().frame;
        if (span <= 0.0f) return kf.back().value;
        const float frac = std::min(1.0f, (frame - kf.back().frame) / span);
        return kf.back().value + frac * (kf.front().value - kf.back().value);
    }
    if (frame >= kf.back().frame) return kf.back().value;

    auto it = std::lower_bound(kf.begin(), kf.end(), frame,
                               [](const PropertyAnimKeyframe& k, float f) { return k.frame < f; });
    auto prev = it - 1;
    const float span = it->frame - prev->frame;
    if (span <= 0.0f) return prev->value;
    const float frac = (frame - prev->frame) / span;
    return prev->value + frac * (it->value - prev->value);
}

} // namespace wallpaper
