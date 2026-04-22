#include <doctest.h>
#include "SceneTickHelpers.h"

using wallpaper::ComputeTickFrametime;

TEST_SUITE("ComputeTickFrametime") {
    TEST_CASE("first tick returns fallback when lastMs < 0") {
        CHECK(ComputeTickFrametime(1000, -1, 0.008, 250) == doctest::Approx(0.008));
        CHECK(ComputeTickFrametime(0, -1, 0.033, 500) == doctest::Approx(0.033));
        CHECK(ComputeTickFrametime(999999, -1, 0.500, 2000) == doctest::Approx(0.500));
    }

    TEST_CASE("returns real dt in seconds for normal ticks") {
        CHECK(ComputeTickFrametime(108, 100, 0.008, 250) == doctest::Approx(0.008));
        CHECK(ComputeTickFrametime(133, 100, 0.033, 500) == doctest::Approx(0.033));
        CHECK(ComputeTickFrametime(600, 100, 0.500, 2000) == doctest::Approx(0.500));
    }

    TEST_CASE("zero-length interval returns 1ms, not zero") {
        // Guards against divide-by-zero in scripts doing v/frametime.
        CHECK(ComputeTickFrametime(100, 100, 0.008, 250) == doctest::Approx(0.001));
    }

    TEST_CASE("negative dt (clock went backwards) returns 1ms floor") {
        CHECK(ComputeTickFrametime(90, 100, 0.008, 250) == doctest::Approx(0.001));
    }

    TEST_CASE("clamps large stalls to the configured ceiling") {
        // 10 second stall — scripts must not suddenly see 10s of sim time.
        CHECK(ComputeTickFrametime(10100, 100, 0.008, 250) == doctest::Approx(0.250));
        CHECK(ComputeTickFrametime(10100, 100, 0.033, 500) == doctest::Approx(0.500));
        CHECK(ComputeTickFrametime(10100, 100, 0.500, 2000) == doctest::Approx(2.000));
    }

    TEST_CASE("regression: 120Hz property tick must not report 33ms") {
        // This is the bug that made dino_run run ~4× too fast.  A property
        // timer firing every 8ms used to hardcode engine.frametime = 0.033.
        // With real dt, an 8ms interval must produce 0.008s, not 0.033s.
        double dt = ComputeTickFrametime(108, 100, 0.008, 250);
        CHECK(dt == doctest::Approx(0.008));
        CHECK(dt < 0.033);
    }

    TEST_CASE("lastMs==0 is a valid prior tick, not the no-previous sentinel") {
        // Boundary: `lastMs < 0` → `<=` would misclassify the lastMs==0 case
        // as "no previous tick" and return fallback, skewing the first real
        // delta after a scene init that picked 0 as the starting timer value.
        CHECK(ComputeTickFrametime(100, 0, 0.008, 250) == doctest::Approx(0.100));
        CHECK(ComputeTickFrametime(50, 0, 0.033, 500) == doctest::Approx(0.050));
    }
}
