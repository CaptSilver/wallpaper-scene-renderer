#include <doctest.h>

#include "Audio/AudioCapture.h"
#include "Audio/AudioAnalyzer.h"

#include <memory>

using namespace wallpaper::audio;

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
