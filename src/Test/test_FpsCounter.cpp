#include <doctest.h>
#include <thread>
#include <chrono>
#include <cstdlib>

#include "Utils/FpsCounter.h"

using wallpaper::FpsCounter;
using namespace std::chrono_literals;

TEST_SUITE("FpsCounter") {
    TEST_CASE("histogram disabled without env returns empty snapshot") {
        // histogramEnabled() reads the env per call, so unsetenv suffices —
        // no need to worry about test ordering or a cached first observation.
        ::unsetenv("WEKDE_DEBUG_FRAMETIME");
        FpsCounter c;
        for (int i = 0; i < 100; ++i) {
            c.RegisterFrame();
            std::this_thread::sleep_for(1ms);
        }
        auto snap = c.CollectHistogram();
        CHECK(snap.sampleCount == 0u);
    }

    TEST_CASE("histogram bins frame intervals into 2ms-wide buckets") {
        ::setenv("WEKDE_DEBUG_FRAMETIME", "1", 1);
        FpsCounter c;
        // Drive 50 RegisterFrame() calls with ~1ms spacing.  Most intervals
        // should land in bucket 0 (0-2 ms); some scheduler jitter pushes a
        // few into bucket 1, which is fine — the assertion allows that.
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(1ms);
            c.RegisterFrame();
        }
        auto snap = c.CollectHistogram();
        CHECK(snap.sampleCount == 50u);
        CHECK(snap.buckets[0] >= 35u);
        ::unsetenv("WEKDE_DEBUG_FRAMETIME");
    }

    TEST_CASE("histogram computes p95/p99/max from bucket walk") {
        ::setenv("WEKDE_DEBUG_FRAMETIME", "1", 1);
        FpsCounter c;
        // 90 fast frames (~1 ms) + 10 hitch frames (~20 ms).  A 10% hitch
        // rate puts both the 95th and 99th percentile inside the 20 ms
        // bucket: the running accumulator over ascending buckets only
        // crosses 95 once the hitches start landing.  Allows scheduler
        // jitter slack on the lower-bound assertion.
        for (int i = 0; i < 90; ++i) {
            std::this_thread::sleep_for(1ms);
            c.RegisterFrame();
        }
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(20ms);
            c.RegisterFrame();
        }
        auto snap = c.CollectHistogram();
        CHECK(snap.sampleCount == 100u);
        CHECK(snap.p95_ms >= 18u);
        CHECK(snap.p99_ms >= 18u);
        CHECK(snap.max_ms >= 18u);
        ::unsetenv("WEKDE_DEBUG_FRAMETIME");
    }

    TEST_CASE("starts at 0 fps before any frame registered") {
        FpsCounter c;
        CHECK(c.Fps() == 0u);
    }

    TEST_CASE("Fps() stays 0 until the first publish window elapses") {
        // Publish window is 500ms.  Registering a few frames immediately must
        // not move Fps() — the counter only updates the atomic at window end.
        FpsCounter c;
        for (int i = 0; i < 30; i++) c.RegisterFrame();
        CHECK(c.Fps() == 0u);
    }

    TEST_CASE("publishes a non-zero Fps after the window") {
        // Register at ~60Hz for slightly longer than the window, then check.
        // Allow a generous range to absorb scheduler jitter on busy CI.
        FpsCounter c;
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < 600ms) {
            c.RegisterFrame();
            std::this_thread::sleep_for(16ms);
        }
        c.RegisterFrame(); // trigger publish
        unsigned int fps = c.Fps();
        CHECK(fps >= 40u);
        CHECK(fps <= 70u);
    }

    TEST_CASE("Fps() is thread-safe — reader sees consistent value") {
        // Smoke check that concurrent Fps()/RegisterFrame() doesn't tear the
        // u32.  The atomic guarantees this; the test catches accidental
        // regressions if anyone makes m_fps non-atomic again.
        FpsCounter        c;
        std::atomic<bool> stop { false };
        std::thread       reader([&] {
            while (! stop.load()) {
                volatile unsigned int v = c.Fps();
                (void)v;
            }
        });
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < 200ms) {
            c.RegisterFrame();
            std::this_thread::sleep_for(1ms);
        }
        stop.store(true);
        reader.join();
        // We just care that nothing crashed and Fps() is in a sane range.
        CHECK(c.Fps() <= 2000u);
    }
}
