#include <doctest.h>

#include "WPPropertyAnimation.h"

#include <cmath>

using namespace wallpaper;

namespace
{
// Build a trivial 0→1 two-keyframe animation (length=1 frame @ 1 fps → 1s period).
PropertyAnimation makeSimpleAnim(PropertyAnimMode mode = PropertyAnimMode::Loop) {
    PropertyAnimation a;
    a.name         = "flash";
    a.property     = "alpha";
    a.mode         = mode;
    a.fps          = 1.0f;
    a.length       = 1.0f;
    a.keyframes    = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
    a.initialValue = 1.0f;
    return a;
}
} // namespace

TEST_SUITE("WPPropertyAnimation_Evaluate") {
    TEST_CASE("empty keyframes returns initialValue") {
        PropertyAnimation a;
        a.initialValue = 0.25f;
        CHECK(EvaluatePropertyAnimation(a, 0.0) == doctest::Approx(0.25f));
        CHECK(EvaluatePropertyAnimation(a, 10.0) == doctest::Approx(0.25f));
    }

    TEST_CASE("single keyframe returns constant value") {
        PropertyAnimation a;
        a.keyframes = { { 0.0f, 0.5f } };
        a.fps       = 10.0f;
        a.length    = 10.0f;
        CHECK(EvaluatePropertyAnimation(a, 0.0) == doctest::Approx(0.5f));
        CHECK(EvaluatePropertyAnimation(a, 5.0) == doctest::Approx(0.5f));
    }

    TEST_CASE("linear interpolation between two keyframes") {
        auto a = makeSimpleAnim(PropertyAnimMode::Single);
        CHECK(EvaluatePropertyAnimation(a, 0.0) == doctest::Approx(0.0f));
        CHECK(EvaluatePropertyAnimation(a, 0.5) == doctest::Approx(0.5f));
        CHECK(EvaluatePropertyAnimation(a, 1.0) == doctest::Approx(1.0f));
    }

    TEST_CASE("single mode clamps past end") {
        auto a = makeSimpleAnim(PropertyAnimMode::Single);
        CHECK(EvaluatePropertyAnimation(a, 100.0) == doctest::Approx(1.0f));
        CHECK(EvaluatePropertyAnimation(a, -1.0) == doctest::Approx(0.0f));
    }

    TEST_CASE("loop mode wraps every period") {
        auto a = makeSimpleAnim(PropertyAnimMode::Loop);
        // period = 1s. t=1.5 → effective t=0.5 → value 0.5
        CHECK(EvaluatePropertyAnimation(a, 1.5) == doctest::Approx(0.5f));
        // t=2.25 → effective 0.25
        CHECK(EvaluatePropertyAnimation(a, 2.25) == doctest::Approx(0.25f));
    }

    TEST_CASE("mirror mode bounces 0→1→0") {
        auto a = makeSimpleAnim(PropertyAnimMode::Mirror);
        // period=1s one-way, 2s round-trip.  At t=1.0 we're at max (1.0).
        CHECK(EvaluatePropertyAnimation(a, 0.0) == doctest::Approx(0.0f));
        CHECK(EvaluatePropertyAnimation(a, 1.0) == doctest::Approx(1.0f));
        // Reverse phase: t=1.5 → reverse position 0.5 → value 0.5
        CHECK(EvaluatePropertyAnimation(a, 1.5) == doctest::Approx(0.5f));
        CHECK(EvaluatePropertyAnimation(a, 2.0) == doctest::Approx(0.0f));
        // Forward again on next cycle
        CHECK(EvaluatePropertyAnimation(a, 2.5) == doctest::Approx(0.5f));
    }

    TEST_CASE("negative time handled gracefully (loop wraps positive)") {
        auto a = makeSimpleAnim(PropertyAnimMode::Loop);
        // t=-0.5 → folds into +0.5 of period
        CHECK(EvaluatePropertyAnimation(a, -0.5) == doctest::Approx(0.5f));
    }

    TEST_CASE("zero fps falls back to first keyframe") {
        PropertyAnimation a;
        a.fps       = 0.0f;
        a.length    = 10.0f;
        a.keyframes = { { 0.0f, 0.3f }, { 10.0f, 0.9f } };
        CHECK(EvaluatePropertyAnimation(a, 5.0) == doctest::Approx(0.3f));
    }

    TEST_CASE("ParsePropertyAnimMode") {
        CHECK(ParsePropertyAnimMode("loop") == PropertyAnimMode::Loop);
        CHECK(ParsePropertyAnimMode("mirror") == PropertyAnimMode::Mirror);
        CHECK(ParsePropertyAnimMode("single") == PropertyAnimMode::Single);
        CHECK(ParsePropertyAnimMode("") == PropertyAnimMode::Loop);
        CHECK(ParsePropertyAnimMode("bogus") == PropertyAnimMode::Loop);
    }

    // Regression: simulate Red Light Left from the train wallpaper
    // (mode=mirror, fps=2, length=1, keyframes [{0,0},{1,1}]).  1-Hz oscillation.
    TEST_CASE("train Red Light Left mirror oscillates at 1 Hz") {
        PropertyAnimation a;
        a.name      = "flash";
        a.property  = "alpha";
        a.mode      = PropertyAnimMode::Mirror;
        a.fps       = 2.0f;
        a.length    = 1.0f;
        a.keyframes = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };

        // one-way period = 0.5s → full bounce = 1s
        CHECK(EvaluatePropertyAnimation(a, 0.0) == doctest::Approx(0.0f));
        CHECK(EvaluatePropertyAnimation(a, 0.25) == doctest::Approx(0.5f));
        CHECK(EvaluatePropertyAnimation(a, 0.5) == doctest::Approx(1.0f));
        CHECK(EvaluatePropertyAnimation(a, 0.75) == doctest::Approx(0.5f));
        CHECK(EvaluatePropertyAnimation(a, 1.0) == doctest::Approx(0.0f));
        // second cycle
        CHECK(EvaluatePropertyAnimation(a, 1.5) == doctest::Approx(1.0f));
    }

    // Exact period boundary for Loop: fmod(period, period) == 0 so the eval
    // should return the first keyframe's value (kills the t<0 → t<=0 mutant on
    // line 71 which would shift to kf.back()).
    TEST_CASE("loop mode at exact period boundary returns front value") {
        auto a = makeSimpleAnim(PropertyAnimMode::Loop);
        CHECK(EvaluatePropertyAnimation(a, 1.0) == doctest::Approx(0.0f));
        CHECK(EvaluatePropertyAnimation(a, 2.0) == doctest::Approx(0.0f));
    }

    // Three-keyframe interpolation that exercises the middle segment: kills
    // the loop-bound mutant (i+1 < size → i+1 <= size) and the subtraction
    // mutants in the fractional computation (frame-kf[i].frame, value diff).
    TEST_CASE("linear interpolation walks middle segment") {
        PropertyAnimation a;
        a.mode      = PropertyAnimMode::Single;
        a.fps       = 10.0f;
        a.length    = 10.0f;
        a.keyframes = { { 0.0f, 0.0f }, { 5.0f, 0.5f }, { 10.0f, 1.0f } };
        // At t=0.7s → frame=7, between kf[1] (5, 0.5) and kf[2] (10, 1.0).
        // frac = (7-5)/(10-5) = 0.4 → value = 0.5 + 0.4*(1.0-0.5) = 0.7
        CHECK(EvaluatePropertyAnimation(a, 0.7) == doctest::Approx(0.7f));
        // At t=0.2s → frame=2, between kf[0] (0, 0) and kf[1] (5, 0.5).
        // frac = 2/5 = 0.4 → value = 0 + 0.4*(0.5-0) = 0.2
        CHECK(EvaluatePropertyAnimation(a, 0.2) == doctest::Approx(0.2f));
        // Exact middle keyframe falls on its own value
        CHECK(EvaluatePropertyAnimation(a, 0.5) == doctest::Approx(0.5f));
    }

    // Non-zero-based keyframe frames: exercises `frame - kf[i].frame`
    // (kills sub→add mutant when kf[i].frame != 0).
    TEST_CASE("interpolation with non-zero starting frame") {
        PropertyAnimation a;
        a.mode      = PropertyAnimMode::Single;
        a.fps       = 10.0f;
        a.length    = 20.0f;
        a.keyframes = { { 5.0f, 0.2f }, { 15.0f, 0.8f } };
        // Before first keyframe → holds front value
        CHECK(EvaluatePropertyAnimation(a, 0.3) == doctest::Approx(0.2f));
        // Midway at frame 10 → (10-5)/(15-5)=0.5 → 0.2 + 0.5*(0.8-0.2)=0.5
        CHECK(EvaluatePropertyAnimation(a, 1.0) == doctest::Approx(0.5f));
        // After last keyframe → holds back value
        CHECK(EvaluatePropertyAnimation(a, 2.0) == doctest::Approx(0.8f));
    }

    // Decreasing keyframes: exercises `kf[i+1].value - kf[i].value` with
    // negative delta (kills sub→add mutant for the value interpolation).
    TEST_CASE("interpolation with decreasing values") {
        PropertyAnimation a;
        a.mode      = PropertyAnimMode::Single;
        a.fps       = 1.0f;
        a.length    = 2.0f;
        a.keyframes = { { 0.0f, 1.0f }, { 2.0f, 0.0f } };
        // Halfway between → (2.0-0)/(2.0-0) applied to 1.0 + 0.5*(0-1.0) = 0.5
        CHECK(EvaluatePropertyAnimation(a, 1.0) == doctest::Approx(0.5f));
        CHECK(EvaluatePropertyAnimation(a, 0.5) == doctest::Approx(0.75f));
    }

    // Zero length: both `period <= 0` guard and `fps <= 0` guard are stressed.
    TEST_CASE("zero length animation holds first keyframe") {
        PropertyAnimation a;
        a.mode      = PropertyAnimMode::Loop;
        a.fps       = 10.0f;
        a.length    = 0.0f;
        a.keyframes = { { 0.0f, 0.4f }, { 1.0f, 0.8f } };
        CHECK(EvaluatePropertyAnimation(a, 0.0) == doctest::Approx(0.4f));
        CHECK(EvaluatePropertyAnimation(a, 5.0) == doctest::Approx(0.4f));
    }

    // Mirror at exact one-way period endpoint: u==period triggers the
    // `u <= period` branch specifically (guards against the <= → < mutant).
    TEST_CASE("mirror at exactly one-way period is max value") {
        auto a = makeSimpleAnim(PropertyAnimMode::Mirror);
        // one-way period = 1s; at t=1.0 value should be exactly kf[1].value
        CHECK(EvaluatePropertyAnimation(a, 1.0) == doctest::Approx(1.0f));
        // Just past: reverse phase kicks in — also 1.0 at the cusp? With u=1+eps,
        // u <= 1 false → t = 2 - 1 - eps ≈ 1, frame ≈ 1, still returns kf[1].
        CHECK(EvaluatePropertyAnimation(a, 1.001) == doctest::Approx(1.0f).epsilon(0.01));
    }

    // ---------------------------------------------------------------------------
    // Boundary mutation killers matching the WPVolumeAnimation set.
    // ---------------------------------------------------------------------------

    TEST_CASE("loop: fps/length give distinct period — cxx_div_to_mul mutant") {
        // fps=30, length=60 → period=2s.  Without the guard below the test,
        // cxx_div_to_mul on `length / fps` would compute 1800 and never wrap.
        PropertyAnimation a;
        a.mode      = PropertyAnimMode::Loop;
        a.fps       = 30.0f;
        a.length    = 60.0f;
        a.keyframes = { { 0, 0 }, { 30, 1.0f } };
        CHECK(EvaluatePropertyAnimation(a, 100.0) == doctest::Approx(0.0f));
    }

    TEST_CASE("loop: fps<=0 skips evaluation (cxx_le_to_lt on fps guard)") {
        PropertyAnimation a;
        a.mode      = PropertyAnimMode::Loop;
        a.fps       = 0.0f; // boundary: fps==0 triggers fallback
        a.length    = 1.0f;
        a.keyframes = { { 0, 0.2f }, { 1, 0.8f } };
        CHECK(EvaluatePropertyAnimation(a, 0.5) == doctest::Approx(0.2f));
    }

    TEST_CASE("duplicate-frame keyframes: span==0 returns first, not NaN") {
        PropertyAnimation a;
        a.mode      = PropertyAnimMode::Loop;
        a.fps       = 1.0f;
        a.length    = 10.0f;
        a.keyframes = { { 0, 0.5f }, { 5, 0.5f }, { 5, 0.9f }, { 10, 1.0f } };
        float v     = EvaluatePropertyAnimation(a, 5.0);
        CHECK(v == doctest::Approx(0.5f));
        CHECK(! std::isnan(v));
    }

    TEST_CASE("loop: negative t wraps to positive via +=period (< boundary at 0)") {
        // Under mutation `t < 0` → `t <= 0`, t==0 would incorrectly add period,
        // shifting the result.  Direct boundary test at exactly time=0.
        PropertyAnimation a;
        a.mode      = PropertyAnimMode::Loop;
        a.fps       = 1.0f;
        a.length    = 10.0f;
        a.keyframes = { { 0, 0.3f }, { 10, 0.7f } };
        CHECK(EvaluatePropertyAnimation(a, 0.0) == doctest::Approx(0.3f));
    }

    TEST_CASE("mirror: negative t wraps via +=T (< boundary at 0)") {
        PropertyAnimation a;
        a.mode      = PropertyAnimMode::Mirror;
        a.fps       = 1.0f;
        a.length    = 10.0f;
        a.keyframes = { { 0, 0.0f }, { 10, 1.0f } };
        CHECK(EvaluatePropertyAnimation(a, 0.0) == doctest::Approx(0.0f));
    }

    // fps==0 AND length==0 boundary: kf.front() return path under the
    // `fps <= 0.0f` guard.  Without the guard (cxx_le_to_lt makes it `< 0`),
    // length/fps becomes 0/0 = NaN, fmod(t, NaN) = NaN, and the comparisons
    // around `frame >= kf.front().frame` collapse so the function returns
    // kf.back().value instead of kf.front().value — observable difference.
    TEST_CASE("zero fps AND zero length forces kf.front() (kills fps<=0 → fps<0 mutant)") {
        PropertyAnimation a;
        a.mode      = PropertyAnimMode::Loop;
        a.fps       = 0.0f; // boundary: fps == 0
        a.length    = 0.0f; // both zero — exposes the `<= 0` vs `< 0` boundary
        a.keyframes = { { 0, 0.25f }, { 10, 0.75f } };
        // Original returns kf.front().value = 0.25.  Mutated lets fps=0
        // through to length/fps = 0/0 = NaN; downstream NaN compares all
        // false, so the function falls to `return kf.back().value` = 0.75.
        CHECK(EvaluatePropertyAnimation(a, 5.0) == doctest::Approx(0.25f));
    }

} // TEST_SUITE
