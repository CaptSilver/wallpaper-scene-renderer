#include <doctest.h>

#include "Particle/AudioRateMultiplier.hpp"

#include <array>
#include <vector>

using wallpaper::audio_reactive::computeRateMultiplier;
using wallpaper::audio_reactive::RateMultiplierParams;
using wallpaper::audio_reactive::RateMultiplierResult;

namespace
{
RateMultiplierParams legacy(uint32_t mode = 1) {
    RateMultiplierParams p;
    p.mode = mode;
    return p;
}
} // namespace

// ============================================================================
// Legacy curve (no WE-shape authored) — pre-existing behavior, retained as the
// default response when an emitter only sets `audioprocessingmode` and none of
// the optional WE-shape keys (frequencystart/end, bounds, exponent, amount).
// ============================================================================
TEST_SUITE("AudioRateMultiplier") {

    TEST_CASE("mode=0 returns 1.0 multiplier and resets smoothed") {
        std::array<float, 16> spec {};
        spec.fill(1.0f);
        auto r = computeRateMultiplier(spec, spec, 0.5, 0.016, legacy(0));
        CHECK(r.multiplier == doctest::Approx(1.0));
        CHECK(r.newSmoothed == doctest::Approx(0.0));
    }

    TEST_CASE("empty spectrum holds previous smoothed and returns 1.0") {
        auto r = computeRateMultiplier({}, {}, 0.7, 0.016, legacy(1));
        CHECK(r.multiplier == doctest::Approx(1.0));
        CHECK(r.newSmoothed == doctest::Approx(0.7));
    }

    TEST_CASE("silent spectrum decays smoothed toward 0") {
        std::array<float, 16> silent {};
        auto r = computeRateMultiplier(silent, silent, 0.5, 0.016, legacy(1));
        CHECK(r.newSmoothed < 0.5);
        CHECK(r.newSmoothed >= 0.0);
    }

    TEST_CASE("loud bass attacks smoothed toward 1.0") {
        std::array<float, 16> spec {};
        for (int i = 0; i < 4; i++) spec[i] = 1.0f;
        auto r = computeRateMultiplier(spec, spec, 0.0, 0.016, legacy(1));
        CHECK(r.newSmoothed > 0.0);
        CHECK(r.newSmoothed <= 1.0);
    }

    TEST_CASE("attack is faster than decay for the same step magnitude") {
        std::array<float, 16> loud {};
        std::array<float, 16> silent {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        auto up   = computeRateMultiplier(loud, loud, 0.0, 0.016, legacy(1));
        auto down = computeRateMultiplier(silent, silent, 1.0, 0.016, legacy(1));
        double attackDelta = up.newSmoothed - 0.0;
        double decayDelta  = 1.0 - down.newSmoothed;
        CHECK(attackDelta > decayDelta);
    }

    TEST_CASE("multiplier at smoothed=0 equals floor 0.4") {
        std::array<float, 16> silent {};
        auto r = computeRateMultiplier(silent, silent, 0.0, 0.016, legacy(1));
        CHECK(r.newSmoothed == doctest::Approx(0.0));
        CHECK(r.multiplier == doctest::Approx(0.4));
    }

    TEST_CASE("multiplier at smoothed=1 approaches ceiling 2.0") {
        std::array<float, 16> loud {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        auto r = computeRateMultiplier(loud, loud, 1.0, 0.016, legacy(1));
        CHECK(r.newSmoothed == doctest::Approx(1.0));
        CHECK(r.multiplier == doctest::Approx(2.0));
    }

    TEST_CASE("multiplier monotonically increases with smoothed") {
        std::array<float, 16> loud {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        auto a = computeRateMultiplier(loud, loud, 0.10, 0.0, legacy(1));
        auto b = computeRateMultiplier(loud, loud, 0.50, 0.0, legacy(1));
        auto c = computeRateMultiplier(loud, loud, 0.90, 0.0, legacy(1));
        CHECK(a.multiplier < b.multiplier);
        CHECK(b.multiplier < c.multiplier);
    }

    TEST_CASE("smoothing converges with constant input") {
        std::array<float, 16> spec {};
        for (int i = 0; i < 4; i++) spec[i] = 0.5f;
        double smoothed = 0.0;
        for (int i = 0; i < 50; i++) {
            auto r   = computeRateMultiplier(spec, spec, smoothed, 0.016, legacy(1));
            smoothed = r.newSmoothed;
        }
        CHECK(smoothed == doctest::Approx(0.5).epsilon(0.01));
    }

    TEST_CASE("only bass band (bins 0..3) drives output by default") {
        std::array<float, 16> highOnly {};
        for (int i = 4; i < 16; i++) highOnly[i] = 1.0f;
        auto r = computeRateMultiplier(highOnly, highOnly, 0.0, 0.016, legacy(1));
        CHECK(r.newSmoothed == doctest::Approx(0.0));
        CHECK(r.multiplier == doctest::Approx(0.4));
    }
}

// ============================================================================
// Channel mode (Gap #1) — WE's `audioprocessingoptions` enum is shared with the
// shader-side AUDIOPROCESSING combo: 1=Left, 2=Right, 3=Stereo.  The particle
// path must source from the matching channel(s); we previously collapsed any
// non-zero mode to "left only".
// ============================================================================
TEST_SUITE("AudioRateMultiplier.channelMode") {

    TEST_CASE("mode=2 ignores left bass") {
        std::array<float, 16> loud {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        std::array<float, 16> silent {};
        auto r = computeRateMultiplier(loud, silent, 0.0, 0.016, legacy(2));
        CHECK(r.newSmoothed == doctest::Approx(0.0));
        CHECK(r.multiplier == doctest::Approx(0.4));
    }

    TEST_CASE("mode=2 responds to right-channel bass") {
        std::array<float, 16> silent {};
        std::array<float, 16> loud {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        auto r = computeRateMultiplier(silent, loud, 0.0, 0.016, legacy(2));
        CHECK(r.newSmoothed > 0.0);
        CHECK(r.multiplier > 0.4);
    }

    TEST_CASE("mode=1 ignores right channel") {
        std::array<float, 16> silent {};
        std::array<float, 16> loud {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        auto r = computeRateMultiplier(silent, loud, 0.0, 0.016, legacy(1));
        CHECK(r.newSmoothed == doctest::Approx(0.0));
    }

    TEST_CASE("mode=3 averages left and right") {
        std::array<float, 16> silent {};
        std::array<float, 16> loud {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        auto half = computeRateMultiplier(silent, loud, 0.0, 0.016, legacy(3));
        auto full = computeRateMultiplier(loud, loud, 0.0, 0.016, legacy(3));
        CHECK(half.newSmoothed > 0.0);
        CHECK(half.newSmoothed < full.newSmoothed);
    }

    TEST_CASE("mode=3 with both empty holds prev and returns 1.0") {
        auto r = computeRateMultiplier({}, {}, 0.6, 0.016, legacy(3));
        CHECK(r.multiplier == doctest::Approx(1.0));
        CHECK(r.newSmoothed == doctest::Approx(0.6));
    }
}

// ============================================================================
// WE-shape curve (Gap #2) — when an emitter authors any of
// audioprocessing{frequencystart,frequencyend,bounds,exponent,amount}, the
// response curve switches from the legacy 0.4..2.0 mapping to the WE-style
//   smoothstep(bounds, mean(spectrum[freqStart..freqEnd])) ^ exp * amount
// ============================================================================
TEST_SUITE("AudioRateMultiplier.weShape") {

    static RateMultiplierParams weShape(uint32_t mode = 1) {
        RateMultiplierParams p;
        p.mode            = mode;
        p.weShapeAuthored = true;
        p.freqStart       = 0;
        p.freqEnd         = 4;
        p.boundsLow       = 0.0f;
        p.boundsHigh      = 1.0f;
        p.exponent        = 1.0f;
        p.amount          = 1.0f;
        return p;
    }

    TEST_CASE("custom freq window: mid-band ignores bass-loud spectrum") {
        std::array<float, 16> bassLoud {};
        for (int i = 0; i < 4; i++) bassLoud[i] = 1.0f;
        std::array<float, 16> midLoud {};
        for (int i = 8; i < 12; i++) midLoud[i] = 1.0f;

        auto p       = weShape();
        p.freqStart  = 8;
        p.freqEnd    = 12;
        auto bassResp = computeRateMultiplier(bassLoud, bassLoud, 0.0, 0.016, p);
        auto midResp  = computeRateMultiplier(midLoud, midLoud, 0.0, 0.016, p);
        CHECK(bassResp.newSmoothed == doctest::Approx(0.0));
        CHECK(midResp.newSmoothed > 0.0);
    }

    TEST_CASE("smoothstep bounds=[0.5, 1.0] gates input below 0.5 to zero output") {
        std::array<float, 16> low {};
        for (int i = 0; i < 4; i++) low[i] = 0.3f;
        auto p        = weShape();
        p.boundsLow   = 0.5f;
        p.boundsHigh  = 1.0f;
        auto r = computeRateMultiplier(low, low, 0.0, 0.016, p);
        CHECK(r.multiplier == doctest::Approx(0.0));
    }

    TEST_CASE("amount=0.5 halves output relative to amount=1.0 at saturation") {
        std::array<float, 16> loud {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        auto p1 = weShape();
        auto p2 = weShape();
        p2.amount = 0.5f;
        auto a = computeRateMultiplier(loud, loud, 1.0, 0.016, p1);
        auto b = computeRateMultiplier(loud, loud, 1.0, 0.016, p2);
        CHECK(a.multiplier == doctest::Approx(1.0));
        CHECK(b.multiplier == doctest::Approx(0.5));
    }

    TEST_CASE("exponent=2.0 reduces mid input vs exponent=1.0") {
        std::array<float, 16> mid {};
        for (int i = 0; i < 4; i++) mid[i] = 0.5f;
        auto pLin = weShape();
        auto pSqr = weShape();
        pSqr.exponent = 2.0f;
        auto a = computeRateMultiplier(mid, mid, 0.5, 0.0, pLin); // dt=0 → smoothed=prev
        auto b = computeRateMultiplier(mid, mid, 0.5, 0.0, pSqr);
        CHECK(a.multiplier == doctest::Approx(0.5));
        CHECK(b.multiplier == doctest::Approx(0.25));
    }

    TEST_CASE("WE-shape silence yields 0 (no legacy 0.4 floor)") {
        std::array<float, 16> silent {};
        auto p = weShape();
        auto r = computeRateMultiplier(silent, silent, 0.0, 0.016, p);
        CHECK(r.multiplier == doctest::Approx(0.0));
    }

    TEST_CASE("WE-shape amount=2.0 reaches ceiling 2.0 at saturation") {
        std::array<float, 16> loud {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        auto p   = weShape();
        p.amount = 2.0f;
        auto r = computeRateMultiplier(loud, loud, 1.0, 0.016, p);
        CHECK(r.multiplier == doctest::Approx(2.0));
    }
}
