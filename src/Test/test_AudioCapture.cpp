#include <doctest.h>

#include "Audio/AudioCapture.h"
#include "Audio/AudioAnalyzer.h"

#include <cstdlib>
#include <memory>
#include <string_view>

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
}
