#include <doctest.h>

#include "Utils/SceneProfiler.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace wallpaper::profiler;

// ─────────────────────────────────────────────────────────────────────────────
// These tests exercise the profiler at the library level.  They are compiled
// into backend_scene_tests regardless of PROFILING build mode, because they
// call the API directly (without the WEK_PROFILE_SCOPE macro) so they are
// always live.  This is intentional: the macro is a convenience wrapper, but
// the underlying machinery must work on its own.
//
// Entry names use distinct string-literal prefixes so repeat test runs (and
// the interaction with other tests) don't collide on the global registry.
// ─────────────────────────────────────────────────────────────────────────────

TEST_SUITE("SceneProfiler") {

TEST_CASE("single sample records count, sum, min, max, last") {
    Reset();
    Entry* e = GetEntry("prof/test/single");
    REQUIRE(e != nullptr);

    e->addSample(1500);

    auto snaps = Collect();
    const Snapshot* found = nullptr;
    for (auto& s : snaps)
        if (s.name == "prof/test/single") found = &s;
    REQUIRE(found != nullptr);

    CHECK(found->count == 1);
    CHECK(found->sum_ns == 1500);
    CHECK(found->min_ns == 1500);
    CHECK(found->max_ns == 1500);
    CHECK(found->last_ns == 1500);
}

TEST_CASE("repeat samples aggregate correctly") {
    Reset();
    Entry* e = GetEntry("prof/test/multi");
    for (uint64_t v : { 100ull, 200ull, 50ull, 1000ull, 300ull }) e->addSample(v);

    auto snaps = Collect();
    const Snapshot* found = nullptr;
    for (auto& s : snaps)
        if (s.name == "prof/test/multi") found = &s;
    REQUIRE(found != nullptr);

    CHECK(found->count == 5);
    CHECK(found->sum_ns == 1650);
    CHECK(found->min_ns == 50);
    CHECK(found->max_ns == 1000);
    CHECK(found->last_ns == 300);
}

TEST_CASE("GetEntry returns the same pointer for repeat names") {
    Reset();
    Entry* a1 = GetEntry("prof/test/stable");
    Entry* a2 = GetEntry("prof/test/stable");
    CHECK(a1 == a2);

    Entry* b = GetEntry("prof/test/stable_other");
    CHECK(a1 != b);
}

TEST_CASE("Reset clears counters but keeps entries") {
    Reset();
    Entry* e = GetEntry("prof/test/reset");
    e->addSample(42);
    e->addSample(99);
    CHECK(e->count.load() == 2);

    Reset();
    // Pointer still valid; fields zeroed.  min goes back to UINT64_MAX (sentinel);
    // Collect() normalises it to 0 for display but the atomic still reads the raw sentinel.
    CHECK(e->count.load() == 0);
    CHECK(e->sum_ns.load() == 0);
    CHECK(e->max_ns.load() == 0);
    CHECK(e->last_ns.load() == 0);

    // Re-adding a sample works.
    e->addSample(7);
    CHECK(e->count.load() == 1);
    CHECK(e->min_ns.load() == 7);
}

TEST_CASE("Collect normalises min for never-sampled entries") {
    Reset();
    (void)GetEntry("prof/test/never_sampled");

    auto snaps = Collect();
    const Snapshot* found = nullptr;
    for (auto& s : snaps)
        if (s.name == "prof/test/never_sampled") found = &s;
    REQUIRE(found != nullptr);
    CHECK(found->count == 0);
    CHECK(found->min_ns == 0); // normalised, not UINT64_MAX
}

TEST_CASE("ScopedTimer records elapsed nanoseconds on destruction") {
    Reset();
    Entry* e = GetEntry("prof/test/scoped");

    {
        ScopedTimer t(e);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    CHECK(e->count.load() == 1);
    // Coarse bounds — sleep_for isn't precise, but 200us should land well
    // above 50us and well under 50ms on any reasonable CI box.
    uint64_t ns = e->sum_ns.load();
    CHECK(ns >= 50'000u);
    CHECK(ns <= 50'000'000u);
}

TEST_CASE("ScopedTimer with nullptr entry is a no-op") {
    ScopedTimer t(nullptr);
    // Should not crash.
    (void)t;
}

TEST_CASE("concurrent samples from many threads aggregate without loss") {
    Reset();
    Entry* e = GetEntry("prof/test/threaded");

    constexpr int       kThreads         = 8;
    constexpr int       kSamplesPerThr   = 1000;
    constexpr uint64_t  kSampleValue     = 10;
    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int i = 0; i < kThreads; i++) {
        ts.emplace_back([e]() {
            for (int j = 0; j < kSamplesPerThr; j++) {
                e->addSample(kSampleValue);
            }
        });
    }
    for (auto& t : ts) t.join();

    CHECK(e->count.load() == kThreads * kSamplesPerThr);
    CHECK(e->sum_ns.load() == kThreads * kSamplesPerThr * kSampleValue);
    CHECK(e->min_ns.load() == kSampleValue);
    CHECK(e->max_ns.load() == kSampleValue);
}

TEST_CASE("Format sorts by total time descending and hides zero-count entries") {
    Reset();
    Entry* slow = GetEntry("prof/test/fmt/slow");
    Entry* fast = GetEntry("prof/test/fmt/fast");
    Entry* idle = GetEntry("prof/test/fmt/idle");

    slow->addSample(9000);
    fast->addSample(100);
    // idle intentionally unused.

    auto snaps = Collect();
    std::string out = Format(snaps);

    auto slow_pos = out.find("prof/test/fmt/slow");
    auto fast_pos = out.find("prof/test/fmt/fast");
    auto idle_pos = out.find("prof/test/fmt/idle");

    REQUIRE(slow_pos != std::string::npos);
    REQUIRE(fast_pos != std::string::npos);
    CHECK(slow_pos < fast_pos);         // sorted descending by sum_ns
    CHECK(idle_pos == std::string::npos); // zero-count rows filtered

    CHECK(out.find("total(ms)") != std::string::npos); // header present
}

TEST_CASE("Format handles empty snapshot list") {
    std::vector<Snapshot> empty;
    auto                  out = Format(empty);
    CHECK(out.find("total(ms)") != std::string::npos);
}

} // TEST_SUITE
