#pragma once
#include <cstdint>
#include <algorithm>

namespace wallpaper
{

/// Compute the real wall-clock frametime (seconds) between two property-script
/// ticks, clamping out pathological cases so scripts never see negative,
/// zero, or multi-second deltas after stalls.
///
/// @param nowMs    current timer reading in milliseconds
/// @param lastMs   previous tick's timer reading in ms (-1 = no previous tick)
/// @param fallback value to return on the first tick (when lastMs < 0)
/// @param clampMs  upper bound on the returned delta in ms (e.g. 250)
/// @return frametime in seconds, suitable for `engine.frametime`
inline double ComputeTickFrametime(int64_t nowMs, int64_t lastMs, double fallback,
                                   int64_t clampMs) {
    if (lastMs < 0) return fallback;
    int64_t dtMs = nowMs - lastMs;
    if (dtMs <= 0) dtMs = 1;
    if (dtMs > clampMs) dtMs = clampMs;
    return static_cast<double>(dtMs) / 1000.0;
}

} // namespace wallpaper
