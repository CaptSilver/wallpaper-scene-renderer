#pragma once
// FramePacing — the frame timer's scheduling math, kept as pure constexpr
// integer functions so the unit tests inject timestamps and assert exact
// values (no clock, no threads).  All times are int64 nanoseconds.
//
// The old timer computed its period as milliseconds(1000/fps): 33.000ms at
// 30fps.  Free-running against a 60Hz display's 33.333ms two-vsync cadence
// that slips one vsync slot every ~1.65s — visible pause-skip stutter.

#include "Core/Literals.hpp"

namespace wallpaper::pacing
{

inline constexpr i64 kNsPerSec = 1'000'000'000;

// fps outside [1,1000]: 0 would divide by zero, >1000 busy-spins the tick
// thread for no visual gain.
constexpr u32 ClampFps(u32 fps) noexcept {
    if (fps == 0) return 1u;
    if (fps > 1000) return 1000u;
    return fps;
}

// Exact tick period, round-to-nearest ns.
constexpr i64 ExactPeriodNs(u32 fps) noexcept {
    const i64 f = static_cast<i64>(ClampFps(fps));
    return (kNsPerSec + f / 2) / f;
}

// Display refresh period from millihertz (59'940 -> 16'683'350ns).
// 0 = unknown (viewer with no reported rate, plasmoid not mapped yet).
constexpr i64 RefreshPeriodNs(u32 refresh_mhz) noexcept {
    if (refresh_mhz == 0) return 0;
    const i64 m = static_cast<i64>(refresh_mhz);
    return (1'000'000'000'000LL + m / 2) / m;
}

} // namespace wallpaper::pacing
