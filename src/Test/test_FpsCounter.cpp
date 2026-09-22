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

    TEST_CASE("histogram bins sub-2ms frame intervals into bucket 0") {
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
        CHECK(snap.sampleCount == 49u);
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
        CHECK(snap.sampleCount == 99u);
        CHECK(snap.p95_ms >= 18u);
        CHECK(snap.p99_ms >= 18u);
        CHECK(snap.max_ms >= 18u);
        ::unsetenv("WEKDE_DEBUG_FRAMETIME");
    }

    TEST_CASE("p95 crosses at the exact target count, not one bucket later") {
        // 95 fast frames + 5 hitch frames, total 100: p95Target is exactly
        // 95, so the running total crosses it the instant the fast frames'
        // bucket(s) finish being counted -- before the hitch bucket is ever
        // added. `acc >= target` catches that boundary; `acc > target`
        // misses it and only crosses once the hitch bucket's count is
        // folded in, reporting the hitch bucket's edge for p95 too.
        ::setenv("WEKDE_DEBUG_FRAMETIME", "1", 1);
        FpsCounter c;
        c.RegisterFrame(); // consume construction-time discard
        for (int i = 0; i < 95; ++i) {
            std::this_thread::sleep_for(1ms);
            c.RegisterFrame();
        }
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(20ms);
            c.RegisterFrame();
        }
        auto snap = c.CollectHistogram();
        CHECK(snap.sampleCount == 100u);
        CHECK(snap.p95_ms < 20u);
        CHECK(snap.p99_ms >= 20u);
        ::unsetenv("WEKDE_DEBUG_FRAMETIME");
    }

    TEST_CASE("widened buckets report p95/p99 above the old 30 ms cap for 54 ms frames") {
        // Old scheme: bucket = min(ms/2, 15), reported as bucket*2 -> max 30ms.
        // A 54ms frame and a 2000ms frame were indistinguishable under that
        // cap. The widened edge table must report at or above the real
        // duration for both p95 and p99 here.
        ::setenv("WEKDE_DEBUG_FRAMETIME", "1", 1);
        FpsCounter c;
        c.RegisterFrame(); // consumes the construction-time discard (m_discardNext defaults true)
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(54ms);
            c.RegisterFrame();
        }
        auto snap = c.CollectHistogram();
        CHECK(snap.sampleCount == 30u);
        CHECK(snap.p95_ms >= 54u);
        CHECK(snap.p99_ms >= 54u);
        ::unsetenv("WEKDE_DEBUG_FRAMETIME");
    }

    TEST_CASE("a mid-range frame lands in its own bucket, not the unbounded catch-all") {
        // The bucket scan walks every edge (2, 3, 5, ..., 500) looking for
        // the first one the frame time is under; a scan that only ever
        // inspects index 0 (e.g. a mis-stepped loop) falls straight through
        // to the unbounded catch-all (15) for everything above bucket 0.
        // 54ms sits between edges[8]=52 and edges[9]=77, so every sample
        // here must land in bucket 9, and 15 must stay empty.
        ::setenv("WEKDE_DEBUG_FRAMETIME", "1", 1);
        FpsCounter c;
        c.RegisterFrame(); // consume construction-time discard
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(54ms);
            c.RegisterFrame();
        }
        auto snap = c.CollectHistogram();
        CHECK(snap.sampleCount == 10u);
        CHECK(snap.buckets[9] == 10u);
        CHECK(snap.buckets[15] == 0u);
        ::unsetenv("WEKDE_DEBUG_FRAMETIME");
    }

    TEST_CASE("a frame lasting exactly a bucket edge lands in the bucket above, not on the edge") {
        // Bucket assignment is `ms < kBucketEdgesMs[i]`, so a frame of
        // EXACTLY 2ms (the smallest edge) belongs in bucket 1 (2 < 3), never
        // bucket 0 (2 < 2 is false). FpsCounter has no injectable clock, and
        // sleep_for routinely overshoots by more than the 1ms window this
        // needs, so busy-spin to the boundary instead -- steady_clock's
        // resolution is far finer than a millisecond, so a tight spin lands
        // on the boundary far more reliably than a sleep would.
        //
        // An occasional overshoot past 2ms proves nothing either way, so
        // retry a few times and only assert once an attempt demonstrably
        // hit the boundary: bucket 1 covers only the single integer value 2,
        // so landing there is proof ms was exactly 2.
        ::setenv("WEKDE_DEBUG_FRAMETIME", "1", 1);
        bool hit_boundary = false;
        for (int attempt = 0; attempt < 50 && ! hit_boundary; ++attempt) {
            FpsCounter c;
            c.RegisterFrame(); // consume construction-time discard
            auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count() < 2) {
            }
            c.RegisterFrame();
            auto snap = c.CollectHistogram();
            if (snap.buckets[1] == 1u) {
                CHECK(snap.buckets[0] == 0u);
                hit_boundary = true;
            }
        }
        REQUIRE(hit_boundary); // never landed exactly on the 2ms boundary in 50 tries
        ::unsetenv("WEKDE_DEBUG_FRAMETIME");
    }

    TEST_CASE("an unbounded-tail outlier reports p99 near its real duration, not the last edge") {
        // 98 fast frames + 2 ~700ms outliers. The catch-all bucket (15) must
        // report the tracked exact max, not a synthetic upper edge, or a
        // multi-second stall would still read back as ~500ms.
        ::setenv("WEKDE_DEBUG_FRAMETIME", "1", 1);
        FpsCounter c;
        c.RegisterFrame();
        for (int i = 0; i < 98; ++i) {
            std::this_thread::sleep_for(1ms);
            c.RegisterFrame();
        }
        for (int i = 0; i < 2; ++i) {
            std::this_thread::sleep_for(700ms);
            c.RegisterFrame();
        }
        auto snap = c.CollectHistogram();
        CHECK(snap.sampleCount == 100u);
        CHECK(snap.p95_ms < 50u);
        CHECK(snap.p99_ms >= 650u);
        ::unsetenv("WEKDE_DEBUG_FRAMETIME");
    }

    TEST_CASE("DiscardNextInterval excludes the following stall from the histogram and max") {
        // A caller (e.g. SET_SCENE) marks the NEXT interval as a known-long
        // gap (compileRenderGraph, asset load) so it never pollutes the
        // frame-time histogram or the tracked max.
        ::setenv("WEKDE_DEBUG_FRAMETIME", "1", 1);
        FpsCounter c;
        c.RegisterFrame();
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(1ms);
            c.RegisterFrame();
        }
        c.DiscardNextInterval();
        std::this_thread::sleep_for(300ms);
        c.RegisterFrame(); // must not become a sample
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(1ms);
            c.RegisterFrame();
        }
        auto snap = c.CollectHistogram();
        CHECK(snap.sampleCount == 10u);
        CHECK(snap.max_ms < 50u);
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
