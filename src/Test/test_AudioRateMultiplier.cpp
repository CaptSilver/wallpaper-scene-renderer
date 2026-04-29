#include <doctest.h>

#include "Particle/AudioRateMultiplier.hpp"

#include <array>
#include <vector>

using wallpaper::audio_reactive::computeRateMultiplier;
using wallpaper::audio_reactive::RateMultiplierResult;

TEST_SUITE("AudioRateMultiplier") {

    TEST_CASE("mode=0 returns 1.0 multiplier and resets smoothed") {
        std::array<float, 16> spec {};
        spec.fill(1.0f);
        auto r = computeRateMultiplier(spec, 0.5, 0.016, 0);
        CHECK(r.multiplier == doctest::Approx(1.0));
        CHECK(r.newSmoothed == doctest::Approx(0.0));
    }

    TEST_CASE("empty spectrum holds previous smoothed and returns 1.0") {
        auto r = computeRateMultiplier({}, 0.7, 0.016, 1);
        CHECK(r.multiplier == doctest::Approx(1.0));
        CHECK(r.newSmoothed == doctest::Approx(0.7));
    }

    TEST_CASE("silent spectrum decays smoothed toward 0") {
        std::array<float, 16> silent {};
        // prev=0.5 with silent input should drop after one tick.
        auto r = computeRateMultiplier(silent, 0.5, 0.016, 1);
        CHECK(r.newSmoothed < 0.5);
        CHECK(r.newSmoothed >= 0.0);
    }

    TEST_CASE("loud bass attacks smoothed toward 1.0") {
        std::array<float, 16> spec {};
        // Bins 0..3 are the bass band.
        for (int i = 0; i < 4; i++) spec[i] = 1.0f;
        auto r = computeRateMultiplier(spec, 0.0, 0.016, 1);
        CHECK(r.newSmoothed > 0.0);
        CHECK(r.newSmoothed <= 1.0);
    }

    TEST_CASE("attack is faster than decay for the same step magnitude") {
        std::array<float, 16> loud {};
        std::array<float, 16> silent {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        // From silence (0.0) attacking up: bass=1.0 step is +1.0
        auto up = computeRateMultiplier(loud, 0.0, 0.016, 1);
        // From full (1.0) decaying down: bass=0.0 step is -1.0
        auto down = computeRateMultiplier(silent, 1.0, 0.016, 1);
        // Attack moved more from prev than decay did.
        double attackDelta = up.newSmoothed - 0.0;       // positive
        double decayDelta  = 1.0 - down.newSmoothed;     // positive
        CHECK(attackDelta > decayDelta);
    }

    TEST_CASE("multiplier at smoothed=0 equals floor 0.4") {
        std::array<float, 16> silent {};
        // mode!=0 with empty bass and prev=0 → smoothed stays 0 → mult = 0.4.
        auto r = computeRateMultiplier(silent, 0.0, 0.016, 1);
        CHECK(r.newSmoothed == doctest::Approx(0.0));
        CHECK(r.multiplier == doctest::Approx(0.4));
    }

    TEST_CASE("multiplier at smoothed=1 approaches ceiling 2.0") {
        std::array<float, 16> loud {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        // Drive to saturation with a long dt.
        auto r = computeRateMultiplier(loud, 1.0, 0.016, 1);
        CHECK(r.newSmoothed == doctest::Approx(1.0));
        CHECK(r.multiplier == doctest::Approx(2.0));
    }

    TEST_CASE("multiplier monotonically increases with smoothed") {
        std::array<float, 16> loud {};
        for (int i = 0; i < 4; i++) loud[i] = 1.0f;
        auto a = computeRateMultiplier(loud, 0.10, 0.0, 1); // dt=0 keeps prev
        auto b = computeRateMultiplier(loud, 0.50, 0.0, 1);
        auto c = computeRateMultiplier(loud, 0.90, 0.0, 1);
        CHECK(a.multiplier < b.multiplier);
        CHECK(b.multiplier < c.multiplier);
    }

    TEST_CASE("smoothing converges with constant input") {
        std::array<float, 16> spec {};
        for (int i = 0; i < 4; i++) spec[i] = 0.5f;
        double smoothed = 0.0;
        for (int i = 0; i < 50; i++) {
            auto r   = computeRateMultiplier(spec, smoothed, 0.016, 1);
            smoothed = r.newSmoothed;
        }
        CHECK(smoothed == doctest::Approx(0.5).epsilon(0.01));
    }

    TEST_CASE("only bass band (bins 0..3) drives output") {
        std::array<float, 16> highOnly {};
        for (int i = 4; i < 16; i++) highOnly[i] = 1.0f;
        auto r = computeRateMultiplier(highOnly, 0.0, 0.016, 1);
        // Bass bins are zero; smoothed should stay at 0.
        CHECK(r.newSmoothed == doctest::Approx(0.0));
        CHECK(r.multiplier == doctest::Approx(0.4));
    }
}
