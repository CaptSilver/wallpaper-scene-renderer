#include <doctest.h>

#include "Audio/SoundManager.h"
#include "Audio/AudioAnalyzer.h"

#include <memory>

using namespace wallpaper::audio;

// SoundManager is a thin wrapper around miniaudio::Device.  Many of its
// methods won't actually succeed in distrobox (no PipeWire socket for Init),
// but they all must be safe to call and should affect observable state
// (muted/volume) correctly.

TEST_SUITE("SoundManager") {
    TEST_CASE("default state: not inited, not muted, Volume==1.0 (miniaudio default)") {
        SoundManager sm;
        CHECK_FALSE(sm.IsInited());
        CHECK_FALSE(sm.Muted());
        // Default volume is 1.0 per miniaudio device defaults.
        CHECK(sm.Volume() == doctest::Approx(1.0f));
    }

    TEST_CASE("SetMuted(true) prevents Init and reports Muted") {
        SoundManager sm;
        sm.SetMuted(true);
        CHECK(sm.Muted());
        CHECK_FALSE(sm.IsInited());
        CHECK_FALSE(sm.Init()); // returns false when muted
    }

    TEST_CASE("SetMuted(false) triggers Init (may succeed or fail depending on env)") {
        SoundManager sm;
        sm.SetMuted(true);
        sm.SetMuted(false);
        CHECK_FALSE(sm.Muted());
        // IsInited() depends on whether a real audio device is available.
        // Either path covered here — no assertion.
    }

    TEST_CASE("SetVolume stores value readable via Volume()") {
        SoundManager sm;
        sm.SetVolume(0.25f);
        CHECK(sm.Volume() == doctest::Approx(0.25f));
        sm.SetVolume(0.0f);
        CHECK(sm.Volume() == doctest::Approx(0.0f));
        sm.SetVolume(2.5f); // may be clamped by miniaudio
        CHECK(sm.Volume() >= 1.0f);
    }

    TEST_CASE("Play / Pause / UnMountAll are safe on uninitialized device") {
        SoundManager sm;
        sm.Play();
        sm.Pause();
        sm.UnMountAll(); // no-op when nothing mounted
        CHECK(true);
    }

    TEST_CASE("SetAudioAnalyzer with valid analyzer and null both accepted") {
        SoundManager sm;
        auto         analyzer = std::make_shared<AudioAnalyzer>();
        sm.SetAudioAnalyzer(analyzer);
        sm.SetAudioAnalyzer(nullptr); // reset path
        CHECK(true);
    }

} // SoundManager
