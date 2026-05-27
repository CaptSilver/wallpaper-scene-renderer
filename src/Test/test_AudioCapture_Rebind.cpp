#include <doctest.h>

#include "Audio/RebindRetry.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace wallpaper::audio;
using namespace std::chrono_literals;

namespace
{
// Fixture mirrors the rebind-side state in AudioCapture::Impl that
// RetryOpenWithBackoff observes: shutdown flag, rebindPending flag, the
// CV that wakes both, plus the per-attempt result the stub returns.
struct RebindFixture {
    std::mutex                                    mu;
    std::condition_variable                       cv;
    std::atomic<bool>                             shutdown { false };
    std::atomic<bool>                             rebindPending { false };
    // Each call to openFn pops a result from the front of this vector.
    // false = transient failure; true = success.
    std::vector<bool>                             openResults;
    std::vector<int>                              openCallTimestampsMs;
    std::chrono::steady_clock::time_point         t0 = std::chrono::steady_clock::now();
    int                                           openCalls { 0 };

    bool openFn(const std::string& /*sink*/) {
        openCallTimestampsMs.push_back(static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count()));
        if (openCalls >= (int)openResults.size()) return false;
        return openResults[openCalls++];
    }
};
} // namespace

TEST_SUITE("AudioCapture.RetryOpenWithBackoff") {

    // 3 attempts all fail -> caller observes false; 3 backoff waits observed
    // (~50 + 200 + 800 = 1050ms total).
    TEST_CASE("3 transient failures: caller observes false after ~1.05s") {
        RebindFixture f;
        f.openResults = { false, false, false };
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = RetryOpenWithBackoff(
            "some-sink", f.shutdown, f.rebindPending, f.cv, f.mu,
            [&](const std::string& s) { return f.openFn(s); });
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        CHECK(ok == false);
        CHECK(f.openCalls == 3);
        // Total backoff ~= 50 + 200 + 800 = 1050ms; allow slack for scheduler.
        CHECK(elapsedMs >= 1000);
        CHECK(elapsedMs <= 1500);
    }

    // First attempt fails, second succeeds -> caller observes true after
    // exactly one 50ms backoff (no 200ms+ delay).
    TEST_CASE("1 failure then success: caller observes true after ~50ms") {
        RebindFixture f;
        f.openResults = { false, true };
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = RetryOpenWithBackoff(
            "some-sink", f.shutdown, f.rebindPending, f.cv, f.mu,
            [&](const std::string& s) { return f.openFn(s); });
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        CHECK(ok == true);
        CHECK(f.openCalls == 2);
        CHECK(elapsedMs >= 50);
        // First backoff (50ms) + second open call; never reaches the 200ms backoff.
        CHECK(elapsedMs < 220);
    }

    // Newer rebindPending arriving during backoff: caller observes false +
    // returns early so the outer RebindLoop can pick up the new target.
    TEST_CASE("rebindPending during backoff: early-return after 1 attempt") {
        RebindFixture f;
        f.openResults = { false, true, true }; // attempt 2 WOULD succeed
        std::thread waker([&] {
            // Wake during the first 50ms backoff (after ~20ms).
            std::this_thread::sleep_for(20ms);
            f.rebindPending = true;
            f.cv.notify_all();
        });
        bool ok = RetryOpenWithBackoff(
            "some-sink", f.shutdown, f.rebindPending, f.cv, f.mu,
            [&](const std::string& s) { return f.openFn(s); });
        waker.join();
        CHECK(ok == false);                    // caller signals "abort, no recovery"
        CHECK(f.openCalls == 1);               // attempt 2 never ran
        CHECK(f.rebindPending.load() == true); // outer loop will see the wake
    }

    // Shutdown during backoff: caller observes false + returns immediately.
    TEST_CASE("shutdown during backoff: early-return, attempt 2 never runs") {
        RebindFixture f;
        f.openResults = { false, true };
        std::thread waker([&] {
            std::this_thread::sleep_for(20ms);
            f.shutdown = true;
            f.cv.notify_all();
        });
        bool ok = RetryOpenWithBackoff(
            "some-sink", f.shutdown, f.rebindPending, f.cv, f.mu,
            [&](const std::string& s) { return f.openFn(s); });
        waker.join();
        CHECK(ok == false);
        CHECK(f.openCalls == 1);
    }

    // Immediate success on first call: zero backoff observed; the helper
    // collapses to a single openFn() call.
    TEST_CASE("first attempt succeeds: zero backoff, exactly 1 open call") {
        RebindFixture f;
        f.openResults = { true };
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = RetryOpenWithBackoff(
            "some-sink", f.shutdown, f.rebindPending, f.cv, f.mu,
            [&](const std::string& s) { return f.openFn(s); });
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        CHECK(ok == true);
        CHECK(f.openCalls == 1);
        CHECK(elapsedMs < 50);
    }
} // TEST_SUITE AudioCapture.RetryOpenWithBackoff
