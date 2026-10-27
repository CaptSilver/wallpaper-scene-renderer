#include <doctest.h>

#include "Audio/RebindRetry.hpp"
#include "Utils/Logging.h"

#include <array>
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

// A first wait long enough that the waker thread lands inside it however late
// the scheduler runs it; a broken early-return shows up as a slow failure.
constexpr std::array kWakeWithinBackoff { 10000 };

TEST_SUITE("AudioCapture.RetryOpenWithBackoff") {
    // Every attempt fails -> caller observes false.  Each wait is followed by
    // another attempt, so there is one more attempt than waits and the helper
    // gives up right after the last one instead of sleeping first.
    TEST_CASE("all attempts fail: an attempt after every wait, no wait after the last") {
        RebindFixture f;
        f.openResults = { false, false, false, false };
        const std::array schedule { 20, 40, 400 };
        bool             ok = RetryOpenWithBackoff(
            "some-sink",
            f.shutdown,
            f.rebindPending,
            f.cv,
            f.mu,
            [&](const std::string& s) {
                return f.openFn(s);
            },
            schedule);
        // Same clock as the fixture's attempt timestamps.
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - f.t0)
                                   .count();
        CHECK(ok == false);
        CHECK(f.openCalls == 4);
        REQUIRE(f.openCallTimestampsMs.size() == 4);
        // The last attempt comes after all 460ms of waiting...
        CHECK(f.openCallTimestampsMs.back() >= 460);
        // ...and nothing is slept after it (a trailing wait would be 400ms).
        CHECK(elapsedMs - f.openCallTimestampsMs.back() < 200);
    }

    // The give-up line is what someone reads in the journal when a wallpaper
    // has gone deaf, so the attempt count it reports has to be the real one.
    TEST_CASE("giving up logs the number of attempts actually made") {
        static std::vector<std::string> lines;
        lines.clear();
        wallpaper_log_test::setSink([](int, const char* msg) {
            lines.emplace_back(msg);
        });

        RebindFixture    f;
        const std::array schedule { 1, 1, 1 };
        RetryOpenWithBackoff(
            "some-sink",
            f.shutdown,
            f.rebindPending,
            f.cv,
            f.mu,
            [&](const std::string& s) {
                return f.openFn(s);
            },
            schedule);
        wallpaper_log_test::setSink(nullptr);

        CHECK(f.openCallTimestampsMs.size() == 4);
        bool reported = false;
        for (const auto& line : lines)
            if (line.find("failed after 4 attempts") != std::string::npos) reported = true;
        CHECK(reported);
    }

    // A Bluetooth headset's sink shows up well before PulseAudio can open its
    // monitor: about 1.7s in a captured log of plasmashell crashing on connect.
    // Retrying for less than that gives up while the headset is still coming up.
    TEST_CASE("production schedule keeps retrying for longer than a headset takes to come up") {
        int totalMs = 0;
        for (int ms : kRebindBackoffMs) totalMs += ms;
        CHECK(totalMs >= 2000);
        // The first retry stays quick for the common case: a sink that was
        // only busy for a moment.
        CHECK(kRebindBackoffMs.front() <= 50);
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
            // Wake during the first backoff (after ~20ms).
            std::this_thread::sleep_for(20ms);
            f.rebindPending = true;
            f.cv.notify_all();
        });
        bool        ok = RetryOpenWithBackoff(
            "some-sink",
            f.shutdown,
            f.rebindPending,
            f.cv,
            f.mu,
            [&](const std::string& s) {
                return f.openFn(s);
            },
            kWakeWithinBackoff);
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
        bool        ok = RetryOpenWithBackoff(
            "some-sink",
            f.shutdown,
            f.rebindPending,
            f.cv,
            f.mu,
            [&](const std::string& s) {
                return f.openFn(s);
            },
            kWakeWithinBackoff);
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

TEST_SUITE("AudioCapture.ShouldScheduleRebind") {
    TEST_CASE("the default sink moved: rebind") {
        CHECK(ShouldScheduleRebind("headset", "speakers", "speakers", false));
    }

    TEST_CASE("the default sink moved mid-rebind: rebind to the newer one") {
        CHECK(ShouldScheduleRebind("speakers", "headset", "", true));
    }

    TEST_CASE("same sink, already bound: nothing to do") {
        CHECK_FALSE(ShouldScheduleRebind("speakers", "speakers", "speakers", false));
    }

    // The last open of this sink failed (nothing bound).  A later event for
    // the same sink is the chance to try again; dropping it leaves capture
    // deaf until the user picks a different output.
    TEST_CASE("same sink, last open failed: rebind again") {
        CHECK(ShouldScheduleRebind("headset", "headset", "", false));
    }

    // Each event would restart the retries from the first attempt, so a
    // server that emits an event per failed open would spin the worker.
    TEST_CASE("same sink while its rebind is still retrying: leave it be") {
        CHECK_FALSE(ShouldScheduleRebind("headset", "headset", "speakers", true));
    }
}

TEST_SUITE("AudioCapture.ClaimRebindTarget") {
    TEST_CASE("a target that is not bound is claimed and marked in progress") {
        std::string claimed;
        bool        inProgress = false;
        CHECK(ClaimRebindTarget("headset", "speakers", inProgress, claimed));
        CHECK(claimed == "headset");
        CHECK(inProgress);
    }

    TEST_CASE("the target is already bound: nothing to claim") {
        std::string claimed;
        bool        inProgress = false;
        CHECK_FALSE(ClaimRebindTarget("speakers", "speakers", inProgress, claimed));
        CHECK_FALSE(inProgress);
    }

    // The worker claims under the same lock the event callback decides under.
    // If "in progress" only became true later, an event for the same sink in
    // between would restart the retries from the first attempt.
    TEST_CASE("an event for the same sink right after the claim is left alone") {
        std::string claimed;
        bool        inProgress = false;
        REQUIRE(ClaimRebindTarget("headset", "", inProgress, claimed));
        CHECK_FALSE(ShouldScheduleRebind("headset", "headset", "", inProgress));
    }
}

TEST_SUITE("AudioCapture.RecordInitialBind") {
    TEST_CASE("first open with no event yet: target and bound are the opened sink") {
        std::string target;
        std::string bound;
        RecordInitialBind("speakers", target, bound);
        CHECK(target == "speakers");
        CHECK(bound == "speakers");
    }

    // The event subscription is live while Init opens the first device.  A
    // default-sink change in that window must survive, so the worker, once
    // started, sees target != bound and moves to it.
    TEST_CASE("a newer default sink reported during Init is kept") {
        std::string target = "headset";
        std::string bound;
        RecordInitialBind("speakers", target, bound);
        CHECK(target == "headset");
        CHECK(bound == "speakers");
    }
}

TEST_SUITE("AudioCapture.RebindTo") {
    TEST_CASE("successful open records the new sink as bound and ends the rebind") {
        std::mutex  mu;
        std::string bound      = "speakers";
        bool        inProgress = true;
        RebindTo("headset", bound, inProgress, mu, [](const std::string&) {
            return true;
        });
        CHECK(bound == "headset");
        CHECK_FALSE(inProgress);
    }

    // A failed open has already torn the old device down.  If bound kept the
    // old name, switching back to that sink would look like a no-op.
    TEST_CASE("failed open leaves nothing bound, not the previous sink") {
        std::mutex  mu;
        std::string bound      = "speakers";
        bool        inProgress = true;
        RebindTo("headset", bound, inProgress, mu, [](const std::string&) {
            return false;
        });
        CHECK(bound.empty());
        CHECK_FALSE(inProgress);
    }

    TEST_CASE("the rebind stays in progress while the open runs") {
        std::mutex  mu;
        std::string bound;
        bool        inProgress     = true;
        bool        seenDuringOpen = false;
        std::string opened;
        RebindTo("headset", bound, inProgress, mu, [&](const std::string& s) {
            opened = s;
            // The open runs unlocked; the event callback can take mu meanwhile.
            std::lock_guard<std::mutex> lk(mu);
            seenDuringOpen = inProgress;
            return true;
        });
        CHECK(opened == "headset");
        CHECK(seenDuringOpen);
        CHECK_FALSE(inProgress);
    }
}
