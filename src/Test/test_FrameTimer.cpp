#include <doctest.h>
#include "Timer/FrameTimer.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace wallpaper;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace
{
bool waitFor(const std::atomic<int>& counter, int at_least, milliseconds budget) {
    const auto end = steady_clock::now() + budget;
    while (steady_clock::now() < end) {
        if (counter.load() >= at_least) return true;
        std::this_thread::sleep_for(1ms);
    }
    return counter.load() >= at_least;
}
} // namespace

TEST_SUITE("FrameTimer") {
    TEST_CASE("regression: 30fps IdeaTime is 33.333ms, not 33ms") {
        FrameTimer t;
        t.SetRequiredFps(30);
        CHECK(t.RequiredFps() == 30);
        CHECK(t.IdeaTime() == doctest::Approx(0.033333333).epsilon(1e-9));
    }
    TEST_CASE("regression: 60fps IdeaTime is 16.667ms — no more 62.5Hz overdrive") {
        FrameTimer t;
        t.SetRequiredFps(60);
        CHECK(t.IdeaTime() == doctest::Approx(0.016666667).epsilon(1e-9));
    }
    TEST_CASE("fps 0 clamps instead of dividing by zero") {
        FrameTimer t;
        t.SetRequiredFps(0);
        CHECK(t.RequiredFps() == 1);
        CHECK(t.IdeaTime() == doctest::Approx(1.0).epsilon(1e-9));
    }
    TEST_CASE("millihertz refresh snaps the period to the true display grid") {
        FrameTimer t;
        t.SetRequiredFps(30);
        t.SetOutputRefreshMillihertz(59'940);
        // 2 vsyncs of 59.94Hz = 33.3667ms
        CHECK(t.IdeaTime() == doctest::Approx(0.0333667).epsilon(1e-6));
        t.SetOutputRefreshMillihertz(0); // unknown again -> exact fps period
        CHECK(t.IdeaTime() == doctest::Approx(0.033333333).epsilon(1e-9));
    }
    TEST_CASE("busy gate caps in-flight callbacks at 4 and Run() resets the budget") {
        std::atomic<int> fires { 0 };
        FrameTimer t([&fires]() { fires++; });
        t.SetRequiredFps(200); // 5ms grid: gate saturates fast
        t.Run();
        // no FrameBegin/FrameEnd ever runs -> gate must stop at 4
        REQUIRE(waitFor(fires, 4, 2000ms));
        std::this_thread::sleep_for(100ms); // generous: more fires would land here
        CHECK(fires.load() == 4);
        // one completed frame frees exactly one slot
        t.FrameBegin();
        t.FrameEnd();
        CHECK(waitFor(fires, 5, 2000ms));
        // regression: 4 leaked slots used to freeze the wallpaper forever —
        // a Stop/Run cycle (the device-lost recovery path) restores the budget
        t.Stop();
        t.Run();
        CHECK(waitFor(fires, 9, 2000ms));
        t.Stop();
    }
    TEST_CASE("skipped ticks are counted, not replayed") {
        std::atomic<int> fires { 0 };
        FrameTimer t([&fires]() { fires++; });
        t.SetRequiredFps(200);
        t.Run();
        REQUIRE(waitFor(fires, 4, 2000ms));
        std::this_thread::sleep_for(60ms); // ~12 grid points pass while gated
        CHECK(t.SkippedTicks() > 0u);
        t.Stop();
    }
} // TEST_SUITE
