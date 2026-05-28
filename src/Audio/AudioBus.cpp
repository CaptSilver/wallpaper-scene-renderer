#include "Audio/AudioBus.h"
#include "Audio/AudioAnalyzer.h"
#include "Audio/AudioCapture.h"
#include "Utils/Logging.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

namespace wallpaper
{
namespace audio
{
namespace
{

// All bus state lives behind g_mutex.  The Process thread holds the same
// mutex only briefly (to copy out the current analyzer pointer + check
// the shutdown flag), so the 60Hz tick rate is unaffected by subscriber
// churn.
std::mutex                     g_mutex;
std::shared_ptr<AudioAnalyzer> g_analyzer_strong; // bus-internal owning ref
std::weak_ptr<AudioAnalyzer>   g_analyzer_handle; // observation handle for tests
std::unique_ptr<AudioCapture>  g_capture;
// Total subscribers (anyone holding an Acquire-returned shared_ptr).
int                            g_subscriber_count { 0 };
// How many of those subscribers asked for system capture.  When this
// drops to zero, the capture is torn down even if other (playback-tap-only)
// subscribers are still alive.
int                            g_capture_want_count { 0 };
int                            g_init_count { 0 }; // test-only counter (capture opens)
bool                           g_capture_active { false };
std::thread                    g_process_thread;
std::condition_variable        g_process_wake;
bool                           g_process_quit { false };
bool                           g_in_null_mode_for_test { false };

bool isNullCaptureMode() {
    const char* e = std::getenv("WEK_TEST_AUDIO_NULL_CAPTURE");
    return e && e[0] == '1';
}

// 60 Hz Process loop — the ONLY caller of analyzer->Process() in the
// whole process.  Subscribers sample read-side spectrum APIs only,
// preserving the MPSC + lock-free-read invariants.
void processLoop() {
    while (true) {
        std::shared_ptr<AudioAnalyzer> analyzer;
        {
            std::unique_lock<std::mutex> lk(g_mutex);
            g_process_wake.wait_for(lk, std::chrono::milliseconds(16),
                                    [] { return g_process_quit; });
            if (g_process_quit) return;
            analyzer = g_analyzer_strong;
        }
        if (analyzer) analyzer->Process();
    }
}

// Tears down the entire bus.  Must be called with g_mutex unlocked
// (signals + joins the Process thread, which itself needs the mutex
// for its wait_for predicate check).
//
// Production safety: g_capture->Stop() below relies on AudioCapture's
// rebind-thread join completing promptly.  That join is racy if the
// shutdown flag is stored outside the rebind-mutex (see
// AudioCapture::Stop, where we now take the rebind-mutex before
// `shutdown = true; notify_all` to keep the predicate store/check pair
// serialised).  Without that property, the bus dtor could wedge the
// scene unload path indefinitely under suite-ordering / desktop-tear-
// down pressure — the same lost-wakeup window that failed under the
// `after Init failure, no orphan worker thread blocks Stop()` test.
void teardownAll() {
    std::thread joiner;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_process_thread.joinable()) {
            g_process_quit = true;
            g_process_wake.notify_all();
            joiner.swap(g_process_thread);
        }
    }
    if (joiner.joinable()) joiner.join();

    std::lock_guard<std::mutex> lk(g_mutex);
    // AudioCapture holds a shared_ptr<AudioAnalyzer> — destroying the
    // capture first drops that ref so the strong refcount can fall to
    // 0 when we reset our own strong ref below.
    if (g_capture) {
        g_capture->Stop();
        g_capture.reset();
    }
    g_capture_active     = false;
    g_subscriber_count   = 0;
    g_capture_want_count = 0;
    g_analyzer_strong.reset();
    // g_analyzer_handle's weak_ptr will now observe expiry the next
    // time anyone calls .lock() / .expired().
    g_process_quit          = false; // reset for next Acquire cycle
    g_in_null_mode_for_test = false;
}

// Stop the capture without tearing down the analyzer / Process thread.
// Used when the capture_want_count drops to zero but other subscribers
// (e.g. playback-tap users) are still alive.  Must be called with
// g_mutex held.
void stopCaptureLocked() {
    if (! g_capture_active) return;
    if (g_capture) {
        g_capture->Stop();
        g_capture.reset();
    }
    g_capture_active = false;
}

// Custom deleter installed on every Acquire-returned shared_ptr.  The
// deleter does NOT destroy the analyzer — that's owned by the bus's
// internal strong ref.  It decrements both the subscriber count and
// (if this subscriber wanted system capture) the capture-want count;
// when the totals hit zero, the bus tears down the relevant pieces.
//
// IMPORTANT: this deleter may run on any thread (Qt main, render
// thread, scene loader thread).  teardownAll handles that — it joins
// the Process thread without holding g_mutex during the join.
struct SubscriberDeleter {
    bool wantedSystemCapture;
    void operator()(AudioAnalyzer* /*p*/) const {
        bool needTeardown = false;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (g_subscriber_count > 0) {
                if (--g_subscriber_count == 0) needTeardown = true;
            }
            if (wantedSystemCapture && g_capture_want_count > 0) {
                if (--g_capture_want_count == 0 && ! needTeardown) {
                    // Other subscribers still want analyzer access but
                    // nobody wants system capture anymore — release the
                    // capture stream while keeping the bus alive.
                    LOG_INFO("Audio spectrum: last capture-wanter dropped, "
                             "releasing AudioBus system capture");
                    stopCaptureLocked();
                }
            }
        }
        if (needTeardown) {
            LOG_INFO("Audio spectrum: last subscriber dropped, tearing down AudioBus");
            teardownAll();
        }
    }
};

// Opens an AudioCapture (or pretends to, in test mode).  Must be
// called with g_mutex held.  Returns true on success.
bool openCaptureLocked() {
    if (g_capture_active) return true;
    if (g_in_null_mode_for_test) {
        // Test path: don't touch PulseAudio.  Pretend capture is up so
        // HasSystemCapture reports true and TEST_getInitCount
        // increments — the test driver feeds PCM into the analyzer
        // directly via AudioAnalyzer::FeedPcm.
        ++g_init_count;
        g_capture_active = true;
        return true;
    }
    g_capture = std::make_unique<AudioCapture>();
    if (! g_capture->Init(g_analyzer_strong)) {
        g_capture.reset();
        return false;
    }
    ++g_init_count;
    g_capture_active = true;
    return true;
}

} // namespace

std::shared_ptr<AudioAnalyzer> AudioBus::Acquire(bool wantSystemCapture) {
    std::shared_ptr<AudioAnalyzer> handle;
    bool                           need_start_thread = false;
    {
        std::lock_guard<std::mutex> lk(g_mutex);

        // Lazy-init the singleton analyzer on the first Acquire.
        if (! g_analyzer_strong) {
            g_analyzer_strong       = std::make_shared<AudioAnalyzer>();
            g_analyzer_handle       = g_analyzer_strong;
            g_in_null_mode_for_test = isNullCaptureMode();
            need_start_thread       = true;
        }

        // Lazy-init the capture on the first wantSystemCapture=true
        // Acquire.  Capture-init failure (no PulseAudio / no .monitor
        // source) is non-fatal: subscribers still get the analyzer,
        // they just won't see system audio.
        if (wantSystemCapture && ! g_capture_active) {
            if (! openCaptureLocked()) {
                LOG_INFO("Audio spectrum: AudioBus capture init failed, "
                         "subscribers will see analyzer-only mode");
            }
        }

        ++g_subscriber_count;
        if (wantSystemCapture) ++g_capture_want_count;
        // Return a shared_ptr aliased to the bus's analyzer but with
        // our subscriber deleter — drops the refcounts, doesn't actually
        // free the analyzer.
        handle = std::shared_ptr<AudioAnalyzer>(g_analyzer_strong.get(),
                                                SubscriberDeleter { wantSystemCapture });
    }

    if (need_start_thread) {
        // Spawn outside g_mutex — the Process thread immediately tries
        // to take the mutex, so spawning it while still locked would
        // serialize for no reason.
        std::lock_guard<std::mutex> lk(g_mutex);
        if (! g_process_thread.joinable()) {
            g_process_thread = std::thread(processLoop);
            LOG_INFO("Audio spectrum: AudioBus Process thread started (60Hz)");
        }
    }

    return handle;
}

bool AudioBus::HasSystemCapture() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_capture_active;
}

bool AudioBus::TEST_isExpired() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_analyzer_handle.expired();
}

int AudioBus::TEST_getInitCount() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_init_count;
}

void AudioBus::TEST_resetForNextCase() {
    // Force teardown even if a stray subscriber leaked from a previous
    // test (defensive — every test should be self-contained, but
    // doctest case ordering isn't guaranteed under -j).
    teardownAll();
}

} // namespace audio
} // namespace wallpaper
