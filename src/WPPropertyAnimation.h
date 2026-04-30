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
    std::string property; // target scalar property, e.g. "alpha"

    PropertyAnimMode mode { PropertyAnimMode::Loop };
    float            fps { 30.0f };
    float            length { 0.0f }; // in frames
    bool             startPaused { false };

    std::vector<PropertyAnimKeyframe> keyframes;

    // Fallback value when no keyframes or playback hasn't started.
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
