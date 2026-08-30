#include <doctest.h>
#include "Timer/ThreadTimer.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace wallpaper;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace
{
// Tier-2 smoke tests: real thread, condition-waits with generous bounds.
// Precision claims live in the FramePacing suite; here we only assert
// ordering and liveness, never durations.
bool waitFor(const std::atomic<int>& counter, int at_least, milliseconds budget) {
    const auto end = steady_clock::now() + budget;
    while (steady_clock::now() < end) {
        if (counter.load() >= at_least) return true;
        std::this_thread::sleep_for(1ms);
    }
    return counter.load() >= at_least;
}
} // namespace

TEST_SUITE("ThreadTimer") {
    TEST_CASE("first wake is immediate after Start") {
        std::atomic<int> wakes { 0 };
        ThreadTimer t([&](ThreadTimer::Clock::time_point now) {
            wakes++;
            return now + 10s; // park far away after the first wake
        });
        t.Start();
        CHECK(waitFor(wakes, 1, 2000ms));
        t.Stop();
    }
    TEST_CASE("Nudge wakes a parked wait promptly") {
        std::atomic<int> wakes { 0 };
        ThreadTimer t([&](ThreadTimer::Clock::time_point now) {
            wakes++;
            return now + 10s;
        });
        t.Start();
        REQUIRE(waitFor(wakes, 1, 2000ms));
        t.Nudge();
        CHECK(waitFor(wakes, 2, 2000ms));
        t.Stop();
    }
    TEST_CASE("no on_wake runs after Stop returns") {
        std::atomic<int> wakes { 0 };
        ThreadTimer t([&](ThreadTimer::Clock::time_point now) {
            wakes++;
            return now + 1ms;
        });
        t.Start();
        REQUIRE(waitFor(wakes, 3, 2000ms));
        t.Stop();
        const int at_stop = wakes.load();
        // generous settle: Stop() joins, so the count must be final already
        std::this_thread::sleep_for(50ms);
        CHECK(wakes.load() == at_stop);
    }
    TEST_CASE("Start while running and Stop while stopped are no-ops") {
        std::atomic<int> wakes { 0 };
        ThreadTimer t([&](ThreadTimer::Clock::time_point now) {
            wakes++;
            return now + 10s;
        });
        t.Stop();      // never started
        t.Start();
        t.Start();     // second Start ignored
        CHECK(waitFor(wakes, 1, 2000ms));
        CHECK(t.Running());
        t.Stop();
        t.Stop();
        CHECK_FALSE(t.Running());
    }
    TEST_CASE("restart after Stop works") {
        std::atomic<int> wakes { 0 };
        ThreadTimer t([&](ThreadTimer::Clock::time_point now) {
            wakes++;
            return now + 10s;
        });
        t.Start();
        REQUIRE(waitFor(wakes, 1, 2000ms));
        t.Stop();
        t.Start();
        CHECK(waitFor(wakes, 2, 2000ms));
        t.Stop();
    }
} // TEST_SUITE
