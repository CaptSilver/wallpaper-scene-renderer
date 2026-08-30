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
} // TEST_SUITE
