#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>
#include <string>

#include "Utils/Logging.h"

namespace wallpaper {
namespace audio {

// Waits between attempts to open a sink's monitor, about 5.9s in all.  A
// Bluetooth headset's sink is announced well before PulseAudio can open its
// monitor (1.7s in one captured connect), so a short schedule gives up while
// the headset is still coming up.
inline constexpr std::array<int, 5> kRebindBackoffMs { 50, 200, 800, 1600, 3200 };

// Try openFn, and after each failure wait the next step of backoffMs and try
// again, so there is one more attempt than waits and nothing is slept after
// the last one.  The waits sit on the provided CV so a shutdown or a newer
// rebindPending ends them early.  Shutdown returns false immediately;
// rebindPending leaves the flag set so the outer RebindLoop re-enters with the
// newer target.
//
// Returns true if any attempt succeeded; false otherwise (including the
// shutdown / preempted cases).
//
// The openFn indirection is the unit-test seam — production callers pass a
// lambda that forwards to AudioCapture::Impl::OpenMaDevice; tests pass a
// stub that returns canned results.
inline bool RetryOpenWithBackoff(const std::string& sink, const std::atomic<bool>& shutdown,
                                 const std::atomic<bool>& rebindPending,
                                 std::condition_variable& cv, std::mutex& mu,
                                 const std::function<bool(const std::string&)>& openFn,
                                 std::span<const int> backoffMs = kRebindBackoffMs) {
    for (std::size_t attempt = 0;; ++attempt) {
        if (shutdown.load()) return false;
        // The first attempt is covered by the caller's "rebinding to ..." log.
        if (attempt > 0) {
            LOG_INFO("AudioCapture: rebind retry %zu for '%s'", attempt, sink.c_str());
        }
        if (openFn(sink)) return true;
        if (attempt == backoffMs.size()) break;
        {
            std::unique_lock<std::mutex> lk(mu);
            // wait_for returns true when the predicate became true within
            // the timeout (shutdown OR newer rebindPending arrived).
            if (cv.wait_for(lk, std::chrono::milliseconds(backoffMs[attempt]), [&] {
                    return shutdown.load() || rebindPending.load();
                })) {
                // Shutdown, or a newer target queued that the outer RebindLoop
                // picks up when we return false.  Leave rebindPending set.
                return false;
            }
        }
    }
    LOG_INFO("AudioCapture: rebind to '%s' failed after %zu attempts; "
             "capture silent until the next sink event",
             sink.c_str(),
             backoffMs.size() + 1);
    return false;
}

// Whether a sink event should wake the rebind worker.  A new default sink
// always does.  For the same sink, a failed open left nothing bound and a
// later event is the chance to try again, but not while a rebind is still
// working through its retries: restarting it on every event would pace the
// attempts by however fast the server emits events instead of the schedule.
inline bool ShouldScheduleRebind(const std::string& newSink, const std::string& targetSink,
                                 const std::string& boundSink, bool rebindInProgress) {
    if (newSink != targetSink) return true;
    return ! rebindInProgress && newSink != boundSink;
}

// Called by the rebind worker under the same lock the event callback decides
// under: claim the target if it isn't bound yet, and mark the rebind in
// progress in that same step.  Marking it any later leaves a gap in which an
// event for the same sink restarts the retries from the first attempt.
inline bool ClaimRebindTarget(const std::string& targetSink, const std::string& boundSink,
                              bool& inProgress, std::string& claimed) {
    if (targetSink == boundSink) return false;
    claimed    = targetSink;
    inProgress = true;
    return true;
}

// Record Init's first successful open.  The event subscription is already live
// while that open runs, so an event may have set a newer target meanwhile;
// keep it, and the worker moves to it as soon as it starts.
inline void RecordInitialBind(const std::string& openedSink, std::string& targetSink,
                              std::string& boundSink) {
    boundSink = openedSink;
    if (targetSink.empty()) targetSink = openedSink;
}

// One rebind of a target claimed with ClaimRebindTarget: open it and record
// what is bound afterwards.  A failed open has already torn the old device
// down, so nothing is bound then; keeping the old name would make switching
// back to that sink look like a no-op.  openWithRetry runs unlocked (it waits
// on mu's CV); boundSink and inProgress, which the PulseAudio event callback
// reads, are only touched under mu.
inline void RebindTo(const std::string& target, std::string& boundSink, bool& inProgress,
                     std::mutex& mu, const std::function<bool(const std::string&)>& openWithRetry) {
    const bool                  ok = openWithRetry(target);
    std::lock_guard<std::mutex> lk(mu);
    boundSink  = ok ? target : std::string {};
    inProgress = false;
}

} // namespace audio
} // namespace wallpaper
