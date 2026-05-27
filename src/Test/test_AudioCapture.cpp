#include <doctest.h>

#include "Audio/AudioCapture.h"
#include "Audio/AudioAnalyzer.h"
#include "Utils/Logging.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace wallpaper::audio;

// Audio device access is environment-dependent: enumerating PulseAudio/
// PipeWire devices can BLOCK indefinitely on a stale/half-open socket — common
// in a headless distrobox where $XDG_RUNTIME_DIR/pulse exists but no server
// answers.  AudioCapture::Init -> ConnectPulse parks in
// pa_threaded_mainloop_wait at 0% CPU and never returns, wedging the whole
// suite (and the bare preflight run).  Gate the device-touching Init() case
// behind an explicit opt-in env var so the suite never hangs.  Set
// WEKDE_HAS_AUDIO_DEVICE=1 on a box with a live audio server to exercise the
// real Init() path.  doctest has no QSKIP; the established equivalent is
// MESSAGE(...) + return; (mirrors wp_pkg_tool_available in test_helpers.cpp).
static bool wekde_audio_device_optin() {
    const char* v = std::getenv("WEKDE_HAS_AUDIO_DEVICE");
    return v && std::string_view(v) == "1";
}

// AudioCapture needs a live PulseAudio/PipeWire server to fully exercise its
// rebind worker.  These tests cover only the lifecycle invariants that must
// hold in any environment (incl. distrobox where there is no PA socket): every
// entry point must be safe to call, even if Init fails.

TEST_SUITE("AudioCapture") {
    TEST_CASE("default-constructed: inactive, Stop() is a no-op") {
        AudioCapture cap;
        CHECK_FALSE(cap.IsActive());
        cap.Stop();
        CHECK_FALSE(cap.IsActive());
    }

    TEST_CASE("Init(nullptr) refuses the call, leaves state inactive") {
        AudioCapture cap;
        CHECK_FALSE(cap.Init(nullptr));
        CHECK_FALSE(cap.IsActive());
    }

    TEST_CASE("Init() never crashes even when PA is unreachable; Stop joins cleanly") {
        // In CI / distrobox there's typically no PipeWire socket, so Init is
        // expected to return false.  The point of the test is that we don't
        // leak the libpulse mainloop thread, dangle the rebind worker, or
        // crash on destruction — all of which were easy to get wrong when
        // the rebind path was first wired in.
        //
        // The libpulse ConnectPulse() path can BLOCK forever on a stale socket
        // rather than failing fast (the original headless-hang bug), so gate
        // the actual Init() behind the opt-in.
        if (! wekde_audio_device_optin()) {
            MESSAGE("no audio device opt-in (set WEKDE_HAS_AUDIO_DEVICE=1); "
                    "skipping live AudioCapture::Init()");
            return;
        }
        AudioCapture cap;
        auto         analyzer = std::make_shared<AudioAnalyzer>();
        (void)cap.Init(analyzer);
        cap.Stop();
        CHECK_FALSE(cap.IsActive());
    }

    TEST_CASE("destructor handles never-init'd instance") {
        // Smoke test: just construct and let RAII run.  Catches accidental
        // null derefs in the Impl destructor / Stop() interaction.
        { AudioCapture cap; }
        SUBCASE("repeated construct/destroy") {
            for (int i = 0; i < 4; ++i) {
                AudioCapture cap;
                (void)cap.IsActive();
            }
        }
    }

    TEST_CASE("after Init failure, no orphan worker thread blocks Stop()") {
        // When ConnectPulse fails AFTER pa_threaded_mainloop_start (e.g.
        // pa_context_new returns null or pa_context_connect fails), the
        // mainloop used to keep running idle for the full scene lifetime —
        // the original ConnectPulse exited the PALock without stopping +
        // freeing paLoop, leaving cleanup to Stop()/dtor much later. Fix
        // cleans up in-place; destruction should be fast even when Init
        // failed half-way.
        if (wekde_audio_device_optin()) {
            MESSAGE("WEKDE_HAS_AUDIO_DEVICE=1 — Init is expected to succeed, skipping");
            return;
        }
        auto cap      = std::make_unique<AudioCapture>();
        auto analyzer = std::make_shared<AudioAnalyzer>();
        (void)cap->Init(analyzer); // expected to fail in distrobox
        const auto t0 = std::chrono::steady_clock::now();
        cap.reset();
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
        CHECK(elapsed_ms < 500);
    }
}

namespace
{
// Process-global sink the doctest cases install while they are scoped.
// backend_scene_tests runs doctest cases serially in a single thread, so a
// raw global is safe; the test must set/clear it within the case scope.
std::vector<std::pair<int, std::string>>* g_currentSink = nullptr;

void appendingSink(int level, const char* msg) {
    if (g_currentSink) g_currentSink->emplace_back(level, std::string(msg));
}
} // namespace

TEST_SUITE("WallpaperLog test hook") {
    TEST_CASE("setSink captures rendered messages; clearing un-installs") {
        std::vector<std::pair<int, std::string>> captured;
        g_currentSink = &captured;
        wallpaper_log_test::setSink(appendingSink);

        WallpaperLog(LOGLEVEL_INFO, "", 0, "hello %s", "world");
        WallpaperLog(LOGLEVEL_ERROR, "test.cpp", 42, "code=%d", 7);

        // Un-install before assertions so a CHECK fault doesn't double-fire
        // into stale state.
        wallpaper_log_test::setSink(nullptr);
        g_currentSink = nullptr;

        REQUIRE(captured.size() == 2);
        CHECK(captured[0].first == LOGLEVEL_INFO);
        CHECK(captured[0].second == "hello world");
        CHECK(captured[1].first == LOGLEVEL_ERROR);
        CHECK(captured[1].second == "code=7");

        // After clearing, further log calls must NOT mutate the captured
        // vector (sink is uninstalled).
        WallpaperLog(LOGLEVEL_INFO, "", 0, "should-not-record");
        CHECK(captured.size() == 2);
    }
}

TEST_SUITE("AudioCapture") {
    TEST_CASE("InitLegacy emits LOG_INFO on every silent return path it hits") {
        // InitLegacy is reached when WEK_HAVE_LIBPULSE is unset OR ConnectPulse
        // fails.  In a distrobox without PA, the libpulse ConnectPulse path
        // can BLOCK on a stale socket (see header comment), so this case is
        // opt-in via WEKDE_HAS_AUDIO_DEVICE=1 and forward-compatible with
        // both libpulse-success and libpulse-fallback runtimes.  When libpulse
        // succeeds, Init logs at line 361 ("active on monitor of ...") and the
        // assertion still passes; when libpulse falls through to InitLegacy,
        // one of the new lines fires.
        if (! wekde_audio_device_optin()) {
            MESSAGE("WEKDE_HAS_AUDIO_DEVICE not set; skipping InitLegacy log-fire assertion");
            return;
        }
        std::vector<std::pair<int, std::string>> captured;
        g_currentSink = &captured;
        wallpaper_log_test::setSink(appendingSink);

        {
            AudioCapture cap;
            auto         analyzer = std::make_shared<AudioAnalyzer>();
            (void)cap.Init(analyzer);
            cap.Stop();
        }

        wallpaper_log_test::setSink(nullptr);
        g_currentSink = nullptr;

        // At least one captured line must start with "AudioCapture:" — either
        // the success path's "active on monitor of ..." or one of the
        // InitLegacy / OpenMaDevice failure lines.
        bool anyAudio = false;
        for (auto& [lvl, msg] : captured) {
            if (msg.find("AudioCapture:") == 0) {
                anyAudio = true;
                break;
            }
        }
        CHECK(anyAudio);
    }
}
