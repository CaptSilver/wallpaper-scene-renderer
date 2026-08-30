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


// One tick grid: absolute deadlines D(i) = base + round(i*num/den) ns.
// Anchor+index instead of deadline+=period so per-tick rounding never
// accumulates; D(i) is exact on the rational grid for any i.
//   fps grid:      num = 1e9,      den = fps        (period 1e9/fps s)
//   refresh grid:  num = k * 1e12, den = refresh_mhz (period = k vsyncs)
struct TickGrid {
    i64 base_ns { 0 };
    u64 index { 0 }; // index of the next deadline
    i64 num { kNsPerSec };
    i64 den { 15 };

    constexpr i64 DeadlineNs() const noexcept {
        return base_ns + (static_cast<i64>(index) * num + den / 2) / den;
    }
    constexpr i64 ApproxPeriodNs() const noexcept { return (num + den / 2) / den; }
};

// Vsyncs per engine tick when snapping to the display grid:
// round(refresh_hz / fps) with refresh in mHz.
constexpr u32 SnapDivisor(u32 fps, u32 refresh_mhz) noexcept {
    const u32 den = 1000u * ClampFps(fps);
    return (refresh_mhz + den / 2) / den;
}

// Snap only when the display grid divides down to within tol of the
// requested rate (default 2%): accepts 30->29.97-on-59.94, rejects
// 28->30-on-60.  A looser tolerance would silently rewrite the user's fps.
constexpr bool SnapAccepted(u32 fps, u32 refresh_mhz, u32 k,
                            u32 tol_permille) noexcept {
    if (k == 0 || refresh_mhz == 0) return false;
    const i64 target = static_cast<i64>(ClampFps(fps)) * 1000 * static_cast<i64>(k);
    const i64 mhz    = static_cast<i64>(refresh_mhz);
    const i64 diff   = mhz > target ? mhz - target : target - mhz;
    return diff * 1000 <= target * static_cast<i64>(tol_permille);
}

constexpr TickGrid MakeGrid(i64 base_ns, u32 fps, u32 refresh_mhz,
                            u32 tol_permille = 20) noexcept {
    const u32 f = ClampFps(fps);
    const u32 k = SnapDivisor(f, refresh_mhz);
    if (SnapAccepted(f, refresh_mhz, k, tol_permille)) {
        return TickGrid { base_ns, 0, static_cast<i64>(k) * 1'000'000'000'000LL,
                          static_cast<i64>(refresh_mhz) };
    }
    return TickGrid { base_ns, 0, kNsPerSec, static_cast<i64>(f) };
}

// Largest safe index: keep index*num well inside i64 (headroom factor 2).
constexpr u64 RebaseIndexLimit(i64 num) noexcept {
    return static_cast<u64>(4'600'000'000'000'000'000LL / num);
}

// Re-anchor at the current next deadline (index 0).  D() is on-grid, so the
// rebase is phase-exact — no jump, testable with injected large indices.
constexpr void RebaseIfNeeded(TickGrid& g) noexcept {
    if (g.index >= RebaseIndexLimit(g.num)) {
        g.base_ns = g.DeadlineNs();
        g.index   = 0;
    }
}

struct TickPlan {
    bool fire;    // invoke the draw callback for this wake
    u64  skipped; // grid deadlines dropped (missed while stalled or gated)
};

// Advance so DeadlineNs() > now_ns; returns deadlines dropped on the way.
// The coarse jump keeps an 8-hour suspend from looping a million times.
// Jumping against period+1 (a ceiling of the true rational period)
// guarantees the jump undershoots for any stall length, so the loop always
// finishes on the exact grid.
constexpr u64 AdvancePastNow(TickGrid& g, i64 now_ns) noexcept {
    u64 skipped = 0;
    const i64 period = g.ApproxPeriodNs() + 1;
    const i64 behind = now_ns - g.DeadlineNs();
    if (behind > 4 * period) {
        const u64 jump = static_cast<u64>(behind / period);
        g.index += jump;
        skipped += jump;
    }
    while (g.DeadlineNs() <= now_ns) {
        g.index += 1;
        skipped += 1;
    }
    return skipped;
}

// One decision per timer-thread wake.
//   now <  deadline               -> hold (nudge/spurious wake)
//   now >= deadline, slot free    -> fire, next deadline = first grid point > now
//   now >= deadline, gate full    -> skip the fire, same advance — deadlines
//                                    stay ON the grid so recovery never bursts
constexpr TickPlan PlanTick(TickGrid& g, i64 now_ns, bool busy_slot_free) noexcept {
    if (now_ns < g.DeadlineNs()) return { false, 0 };
    const bool fire = busy_slot_free;
    g.index += 1;
    u64 skipped = AdvancePastNow(g, now_ns);
    if (! fire) skipped += 1;
    RebaseIfNeeded(g);
    return { fire, skipped };
}

} // namespace wallpaper::pacing
