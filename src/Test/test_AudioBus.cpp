// Process-singleton AudioBus — Scene + Web wallpapers share one
// AudioCapture + AudioAnalyzer pair instead of each spinning their own
// PulseAudio monitor stream + miniaudio thread + FFT.
//
// Tests run under WEK_TEST_AUDIO_NULL_CAPTURE=1 so the bus pretends the
// capture is active without actually opening PulseAudio/PipeWire (which
// would require a working sound server in the test environment).

#include <doctest.h>
#include "Audio/AudioBus.h"
#include "Audio/AudioAnalyzer.h"

#include <cstdlib>

namespace
{
struct NullCaptureGuard {
    NullCaptureGuard() { setenv("WEK_TEST_AUDIO_NULL_CAPTURE", "1", 1); }
    ~NullCaptureGuard() {
        unsetenv("WEK_TEST_AUDIO_NULL_CAPTURE");
        // Drop any stale bus state from a previous test so the next test
        // sees a clean weak_ptr.  TEST_resetForNextCase blocks until the
        // background Process thread has joined.
        wallpaper::audio::AudioBus::TEST_resetForNextCase();
    }
};
} // namespace

TEST_CASE("AudioBus::Acquire returns same analyzer for concurrent subscribers") {
    NullCaptureGuard g;
    auto             a = wallpaper::audio::AudioBus::Acquire(true);
    auto             b = wallpaper::audio::AudioBus::Acquire(true);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a.get() == b.get());
}

TEST_CASE("AudioBus survives first subscriber drop") {
    NullCaptureGuard g;
    auto             a = wallpaper::audio::AudioBus::Acquire(true);
    auto             b = wallpaper::audio::AudioBus::Acquire(true);
    REQUIRE(a);
    REQUIRE(b);
    a.reset();
    // b still functional; HasData callable without crash.
    CHECK_NOTHROW((void)b->HasData());
}

TEST_CASE("AudioBus expires when last subscriber drops") {
    NullCaptureGuard g;
    {
        auto a = wallpaper::audio::AudioBus::Acquire(true);
        REQUIRE(a);
    }
    // The custom deleter has fired by now.  The bus's weak_ptr observes
    // expiry; TEST_isExpired reports it under the bus mutex.
    CHECK(wallpaper::audio::AudioBus::TEST_isExpired());
}

TEST_CASE("AudioBus upgrade: false-then-true acquirer enables capture") {
    NullCaptureGuard g;
    auto             a = wallpaper::audio::AudioBus::Acquire(false);
    REQUIRE(a);
    CHECK(! wallpaper::audio::AudioBus::HasSystemCapture());
    auto b = wallpaper::audio::AudioBus::Acquire(true);
    REQUIRE(b);
    CHECK(wallpaper::audio::AudioBus::HasSystemCapture());
    CHECK(a.get() == b.get()); // upgrade returns the same shared analyzer
}

TEST_CASE("AudioBus reinit after full teardown mints fresh capture") {
    NullCaptureGuard g;
    int              before = wallpaper::audio::AudioBus::TEST_getInitCount();
    {
        auto a = wallpaper::audio::AudioBus::Acquire(true);
        REQUIRE(a);
    }
    // Bus torn down — next Acquire opens a fresh capture.
    auto b = wallpaper::audio::AudioBus::Acquire(true);
    REQUIRE(b);
    int after = wallpaper::audio::AudioBus::TEST_getInitCount();
    CHECK(after == before + 2); // one open at first Acquire, one at re-Acquire
}
