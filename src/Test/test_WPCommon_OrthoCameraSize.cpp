#include <doctest.h>
#include "WPCommon.hpp"

using namespace wallpaper;

TEST_SUITE("WPCommon.ComputeOrthoCameraSize") {
    // Regression for Spiraling Sunlight (1850341785) and Astronaut
    // (2530355779) SIGFPE crashes: their user-property `zoom` defaults to
    // 0.5; pre-fix, `(i32)0.5 = 0` and `context.ortho_w / 0` killed the
    // load thread silently.

    TEST_CASE("zoom 1.0 → native size (no scaling)") {
        auto [w, h] = ComputeOrthoCameraSize(1920, 1080, 1.0f);
        CHECK(w == 1920);
        CHECK(h == 1080);
    }

    TEST_CASE("zoom 0.5 → camera shows 2× world (no SIGFPE)") {
        // The crashing case.  Math: 5760 / 0.5 = 11520.
        auto [w, h] = ComputeOrthoCameraSize(5760, 1080, 0.5f);
        CHECK(w == 11520);
        CHECK(h == 2160);
    }

    TEST_CASE("zoom 2.0 → camera shows half (zoomed in)") {
        auto [w, h] = ComputeOrthoCameraSize(1920, 1080, 2.0f);
        CHECK(w == 960);
        CHECK(h == 540);
    }

    TEST_CASE("zoom 0.0 falls back to native (no SIGFPE)") {
        // The original integer-division-by-zero trigger.
        auto [w, h] = ComputeOrthoCameraSize(1920, 1080, 0.0f);
        CHECK(w == 1920);
        CHECK(h == 1080);
    }

    TEST_CASE("zoom negative falls back to native") {
        // Defensive: WE never authors negative zoom but a buggy script could.
        auto [w, h] = ComputeOrthoCameraSize(1920, 1080, -1.0f);
        CHECK(w == 1920);
        CHECK(h == 1080);
    }

    TEST_CASE("massive zoom that rounds quotient to 0 falls back to native") {
        // If zoom is so large that the division rounds to zero, don't emit a
        // zero-size camera (downstream code may divide by it).
        auto [w, h] = ComputeOrthoCameraSize(10, 10, 1e9f);
        CHECK(w == 10);
        CHECK(h == 10);
    }

    TEST_CASE("fractional zoom keeps float precision through the divide") {
        // Pre-fix, `(i32)1.5 = 1` so the zoom was a no-op.  Post-fix, the
        // divisor is float so the result reflects true semantics.
        auto [w, h] = ComputeOrthoCameraSize(1920, 1080, 1.5f);
        CHECK(w == 1280);
        CHECK(h == 720);
    }
}
