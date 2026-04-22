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
inline float EvaluateVolumeAnimation(const std::vector<VolumeAnimKeyframe>& keyframes, float fps,
                                     float length, const std::string& mode, double time) {
    if (keyframes.empty()) return 0.0f;
    if (keyframes.size() == 1) return keyframes[0].value;

    double maxTime = (double)length / fps;
    double t       = time;
    if (mode == "loop" && maxTime > 0) {
        t = std::fmod(t, maxTime);
        if (t < 0) t += maxTime;
    } else if (mode == "single") {
        t = std::clamp(t, 0.0, maxTime);
    }

    float frame = (float)(t * fps);

    if (frame <= keyframes.front().frame) return keyframes.front().value;
    if (frame >= keyframes.back().frame) return keyframes.back().value;

    for (size_t i = 0; i + 1 < keyframes.size(); i++) {
        if (frame >= keyframes[i].frame && frame <= keyframes[i + 1].frame) {
            float span = keyframes[i + 1].frame - keyframes[i].frame;
            if (span <= 0) return keyframes[i].value;
            float frac = (frame - keyframes[i].frame) / span;
            return keyframes[i].value + frac * (keyframes[i + 1].value - keyframes[i].value);
        }
    }
    return keyframes.back().value;
}

} // namespace wallpaper
