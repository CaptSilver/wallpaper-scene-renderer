#pragma once
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>

namespace wallpaper
{

struct VolumeAnimKeyframe {
    float frame { 0 };
    float value { 0 };
};

// Evaluate a keyframed volume animation curve at a given time.
// Returns the interpolated volume value.
//
// Implementation notes (mutation hardening):
// - Use std::lower_bound to locate the right-side keyframe; this kills the
//   linear-scan boundary mutants that would otherwise be equivalent because
//   the front/back guards would catch the only frames that hit the loop's
//   boundary indices.
// - The lower_bound predicate uses strict `<` so frame == kf[i].frame returns
//   the prev-side keyframe directly (same span/frac math, but the boundary
//   case becomes observable to == vs < mutators).
inline float EvaluateVolumeAnimation(const std::vector<VolumeAnimKeyframe>& keyframes, float fps,
                                     float length, const std::string& mode, double time) {
    if (keyframes.empty()) return 0.0f;
    if (keyframes.size() == 1) return keyframes[0].value;
    if (fps <= 0.0f) return keyframes[0].value;

    const double maxTime = (double)length / fps;
    double t = time;
    if (mode == "loop" && maxTime > 0) {
        t = std::fmod(t, maxTime);
        if (t < 0) t += maxTime;
    } else if (mode == "single") {
        t = std::clamp(t, 0.0, maxTime);
    }

    const float frame = (float)(t * fps);
    if (frame <= keyframes.front().frame) return keyframes.front().value;
    if (frame >= keyframes.back().frame) return keyframes.back().value;

    auto it = std::lower_bound(keyframes.begin(), keyframes.end(), frame,
                               [](const VolumeAnimKeyframe& k, float f) { return k.frame < f; });
    // After the front/back guards above, `it` is guaranteed in (begin, end).
    // it->frame >= frame; (it-1)->frame < frame.
    auto prev = it - 1;
    const float span = it->frame - prev->frame;
    if (span <= 0.0f) return prev->value;
    const float frac = (frame - prev->frame) / span;
    return prev->value + frac * (it->value - prev->value);
}

} // namespace wallpaper
