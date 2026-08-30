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
} // TEST_SUITE
