#include <doctest.h>
#include "Timer/FramePacing.hpp"

using namespace wallpaper;
using namespace wallpaper::pacing;

// ===========================================================================
// periods
// ===========================================================================

TEST_SUITE("FramePacing") {
    TEST_CASE("regression: 30fps period is 33'333'333ns, not the truncated 33ms") {
        CHECK(ExactPeriodNs(30) == 33'333'333);
    }
    TEST_CASE("regression: 60fps period is 16'666'667ns, not the truncated 16ms") {
        // the 16ms tick ran the engine at 62.5fps on a 60Hz target
        CHECK(ExactPeriodNs(60) == 16'666'667);
    }
    TEST_CASE("15fps default period is exact") {
        CHECK(ExactPeriodNs(15) == 66'666'667);
    }
    TEST_CASE("fps 0 clamps to 1 instead of dividing by zero") {
        CHECK(ClampFps(0) == 1u);
        CHECK(ExactPeriodNs(0) == 1'000'000'000);
    }
    TEST_CASE("fps above 1000 clamps instead of busy-spinning") {
        CHECK(ClampFps(2000) == 1000u);
        CHECK(ExactPeriodNs(2000) == 1'000'000);
    }
    TEST_CASE("refresh period from millihertz keeps the 59.94Hz fraction") {
        // 1e12 / 59'940 = 16'683'350.0 ns — Math.round(59.94) = 60 destroyed this
        CHECK(RefreshPeriodNs(59'940) == 16'683'350);
        CHECK(RefreshPeriodNs(60'000) == 16'666'667);
    }
    TEST_CASE("refresh 0 means unknown and returns 0") {
        CHECK(RefreshPeriodNs(0) == 0);
    }

    // =======================================================================
    // resolving the rate a caller feeds the grid
    // =======================================================================

    TEST_CASE("an explicit override beats the rate detected from the screen") {
        // the whole point of the flag: test a 59.94Hz panel from a 144Hz desk
        CHECK(ResolveRefreshMhz(143'999, 59'940) == 59'940u);
    }
    TEST_CASE("without an override the detected rate passes through") {
        CHECK(ResolveRefreshMhz(143'999, 0) == 143'999u);
    }
    TEST_CASE("a negative override means unset, not invalid") {
        CHECK(ResolveRefreshMhz(143'999, -1) == 143'999u);
    }
    TEST_CASE("nothing known stays 0 so MakeGrid falls back to the exact fps grid") {
        CHECK(ResolveRefreshMhz(0, 0) == 0u);
        CHECK(MakeGrid(0, 60, ResolveRefreshMhz(0, 0)).den == 60);
    }
    TEST_CASE("a sub-1Hz rate is nonsense and reads as unknown") {
        // glfwGetVideoMode reports refreshRate 0 on some drivers; 0*1000 = 0
        CHECK(ResolveRefreshMhz(0, 0) == 0u);
        CHECK(ResolveRefreshMhz(999, 0) == 0u);
    }
    TEST_CASE("a rate above 1000Hz reads as unknown rather than snapping to garbage") {
        CHECK(ResolveRefreshMhz(1'000'001, 0) == 0u);
    }
    TEST_CASE("an out-of-range override is rejected, not silently used") {
        CHECK(ResolveRefreshMhz(143'999, 5) == 0u);
    }
    TEST_CASE("the 1Hz and 1000Hz bounds are inclusive") {
        CHECK(ResolveRefreshMhz(1'000, 0) == 1'000u);
        CHECK(ResolveRefreshMhz(1'000'000, 0) == 1'000'000u);
    }
    // =======================================================================
    // tick grid
    // =======================================================================

    TEST_CASE("grid deadlines are anchor+index rational — 30fps hits 100ms exactly") {
        TickGrid g = MakeGrid(0, 30, 0);
        CHECK(g.num == kNsPerSec);
        CHECK(g.den == 30);
        g.index = 0; CHECK(g.DeadlineNs() == 0);
        g.index = 1; CHECK(g.DeadlineNs() == 33'333'333);
        g.index = 2; CHECK(g.DeadlineNs() == 66'666'667);
        g.index = 3; CHECK(g.DeadlineNs() == 100'000'000);   // 33.000ms made this 99ms
    }
    TEST_CASE("one million 30fps ticks land on the grid with zero drift") {
        TickGrid g = MakeGrid(0, 30, 0);
        g.index = 3'000'000;
        CHECK(g.DeadlineNs() == 100'000'000'000'000);        // exactly 100'000s
    }
    TEST_CASE("snap: 30fps on a 60.000Hz display is exactly every 2nd vsync") {
        TickGrid g = MakeGrid(0, 30, 60'000);
        CHECK(g.num == 2'000'000'000'000LL);
        CHECK(g.den == 60'000);
        g.index = 1; CHECK(g.DeadlineNs() == 33'333'333);
        g.index = 3; CHECK(g.DeadlineNs() == 100'000'000);
    }
    TEST_CASE("snap: 30fps on a 59.94Hz display follows the true display grid") {
        TickGrid g = MakeGrid(0, 30, 59'940);
        g.index = 1; CHECK(g.DeadlineNs() == 33'366'700);    // 2 * 16'683'350
    }
    TEST_CASE("snap: 60fps on 59.94Hz accepted (0.1% off), engine follows the panel") {
        CHECK(SnapAccepted(60, 59'940, SnapDivisor(60, 59'940), 20));
        TickGrid g = MakeGrid(0, 60, 59'940);
        g.index = 1; CHECK(g.DeadlineNs() == 16'683'350);
    }
    TEST_CASE("snap rejected when it would silently change the user's fps") {
        // 28fps on 60Hz: nearest divisor is 30fps, 7% off — keep exact 28
        CHECK_FALSE(SnapAccepted(28, 60'000, SnapDivisor(28, 60'000), 20));
        TickGrid g = MakeGrid(0, 28, 60'000);
        CHECK(g.num == kNsPerSec);
        CHECK(g.den == 28);
        // 24-on-60 (3:2 pulldown territory) and 25-on-60 likewise stay exact
        CHECK_FALSE(SnapAccepted(24, 60'000, SnapDivisor(24, 60'000), 20));
        CHECK_FALSE(SnapAccepted(25, 60'000, SnapDivisor(25, 60'000), 20));
    }
    TEST_CASE("unknown refresh (0 mHz) falls back to the exact fps grid") {
        TickGrid g = MakeGrid(500, 60, 0);
        CHECK(g.num == kNsPerSec);
        CHECK(g.den == 60);
        CHECK(g.base_ns == 500);
    }
    TEST_CASE("rebase keeps the same next deadline, phase-exact") {
        TickGrid g = MakeGrid(0, 30, 0);
        g.index = RebaseIndexLimit(g.num);
        const i64 before = g.DeadlineNs();
        RebaseIfNeeded(g);
        CHECK(g.index == 0);
        CHECK(g.base_ns == before);
        CHECK(g.DeadlineNs() == before);
    }
    TEST_CASE("rebase limit is small enough that index*num cannot overflow") {
        // worst realistic mhz grid: 1fps on 1000Hz -> k=1000, num=1e15
        CHECK(RebaseIndexLimit(1'000'000'000'000'000LL) >= 1u);
        CHECK(RebaseIndexLimit(kNsPerSec) > 4'000'000'000u);
    }

    // =======================================================================
    // PlanTick
    // =======================================================================

    TEST_CASE("wake before the deadline holds — spurious wakeups are harmless") {
        TickGrid g = MakeGrid(1'000, 30, 0);
        auto plan = PlanTick(g, 999, true);
        CHECK_FALSE(plan.fire);
        CHECK(plan.skipped == 0u);
        CHECK(g.DeadlineNs() == 1'000);
    }
    TEST_CASE("wake at the deadline fires and schedules the next grid point") {
        TickGrid g = MakeGrid(1'000, 30, 0);
        auto plan = PlanTick(g, 1'000, true);
        CHECK(plan.fire);
        CHECK(plan.skipped == 0u);
        CHECK(g.DeadlineNs() == 1'000 + 33'333'333);
    }
    TEST_CASE("saturated busy gate skips the fire but stays on the grid") {
        TickGrid g = MakeGrid(1'000, 30, 0);
        auto plan = PlanTick(g, 1'000, false);
        CHECK_FALSE(plan.fire);
        CHECK(plan.skipped == 1u);
        CHECK(g.DeadlineNs() == 1'000 + 33'333'333);
    }
    TEST_CASE("late wake fires once, drops the missed deadlines, no burst") {
        TickGrid g = MakeGrid(0, 30, 0);
        // woke 3.5 periods late: fire one, deadlines 1..3 are gone
        auto plan = PlanTick(g, 116'666'666, true);
        CHECK(plan.fire);
        CHECK(plan.skipped == 3u);
        CHECK(g.DeadlineNs() == 133'333'333);
    }
    TEST_CASE("multi-hour stall (suspend) lands back on the grid") {
        TickGrid g = MakeGrid(0, 60, 0);
        const i64 eight_hours = 8LL * 3600 * kNsPerSec;
        auto plan = PlanTick(g, eight_hours, true);
        CHECK(plan.fire);
        const i64 next = g.DeadlineNs();
        CHECK(next > eight_hours);
        CHECK(next - eight_hours <= 2 * g.ApproxPeriodNs());
        CHECK(g.base_ns == 0); // still the original anchor — no rebase fired
    }
} // TEST_SUITE
