#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

#include "Utils/Logging.h"

namespace wallpaper {
namespace audio {

// Retry the open-device call up to 3 times with exponential backoff
// (50ms, 200ms, 800ms) when an attempt fails.  The first attempt is
// immediate (no initial backoff); subsequent attempts wait on the provided
// CV so a shutdown or a newer rebindPending wake aborts the backoff early.
// Shutdown returns false immediately; rebindPending leaves the flag set so
// the outer RebindLoop re-enters with the newer target.
//
// Returns true if any attempt succeeded; false otherwise (including the
// shutdown / preempted cases).
//
// The openFn indirection is the unit-test seam — production callers pass a
// lambda that forwards to AudioCapture::Impl::OpenMaDevice; tests pass a
// stub that returns canned results.
inline bool RetryOpenWithBackoff(
    const std::string&                             sink,
    const std::atomic<bool>&                       shutdown,
    const std::atomic<bool>&                       rebindPending,
    std::condition_variable&                       cv,
    std::mutex&                                    mu,
    const std::function<bool(const std::string&)>& openFn) {

    // Backoff schedule between attempts: 50, 200, 800 (each step = base <<
    // (2 * attempt)).  3 attempts total.
    constexpr int kRetryCount    = 3;
    constexpr int kBackoffMsBase = 50;
    for (int attempt = 0; attempt < kRetryCount; ++attempt) {
        if (shutdown.load()) return false;
        // Log the retry attempts (2..N) — the initial attempt is the
        // caller's "rebinding to ..." log already.
        if (attempt > 0) {
            LOG_INFO("AudioCapture: rebind retry %d for '%s'",
                     attempt, sink.c_str());
        }
        if (openFn(sink)) return true;
        const int waitMs = kBackoffMsBase << (2 * attempt); // 50, 200, 800
        {
            std::unique_lock<std::mutex> lk(mu);
            // wait_for returns true when the predicate became true within
            // the timeout (shutdown OR newer rebindPending arrived).
            if (cv.wait_for(lk, std::chrono::milliseconds(waitMs), [&] {
                    return shutdown.load() || rebindPending.load();
                })) {
                if (shutdown.load()) return false;
                // Newer target queued — outer RebindLoop will pick it up
                // when we return false.  Leave rebindPending set.
                return false;
            }
        }
    }
    LOG_INFO(
        "AudioCapture: rebind to '%s' failed after %d retries; "
        "capture silent until next sink change",
        sink.c_str(), kRetryCount);
    return false;
}

} // namespace audio
} // namespace wallpaper
