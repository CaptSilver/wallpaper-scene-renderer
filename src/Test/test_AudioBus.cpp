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

#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

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

TEST_CASE("the bus thread runs no FFT while nothing declares a spectrum consumer") {
    // The FFT costs a 512-sample stereo transform 60 times a second.  A
    // wallpaper with no spectrum uniform, no audio-reactive particle and no
    // SceneScript audio buffer reads nothing, so the bus must stay idle
    // rather than burn that work for every scene on the desktop.
    NullCaptureGuard g;
    auto             a = wallpaper::audio::AudioBus::Acquire(true);
    REQUIRE(a);

    // Two full FFT windows are queued and ready to consume.
    std::vector<float> pcm(2048, 0.25f);
    a->FeedPcm(pcm.data(), 1024, 2);

    // Ten bus ticks' worth of wall clock (the loop wakes every 16 ms).
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    CHECK(a->WindowsProcessedForTest() == 0);
}

TEST_CASE("a spectrum lease starts the bus FFT, dropping it stops it again") {
    NullCaptureGuard g;
    auto             a = wallpaper::audio::AudioBus::Acquire(true);
    REQUIRE(a);

    auto lease = wallpaper::audio::AudioBus::AcquireSpectrumConsumer();
    REQUIRE(lease);

    std::vector<float> pcm(4096, 0.25f);
    a->FeedPcm(pcm.data(), 2048, 2);

    // Poll rather than sleep a fixed span: the bus wakes every 16 ms but the
    // test host's scheduling is not ours to assume.
    uint64_t processed = 0;
    for (int i = 0; i < 200 && processed == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        processed = a->WindowsProcessedForTest();
    }
    REQUIRE(processed > 0);

    lease.reset();
    // Give the loop a moment to observe the withdrawal, then confirm fresh
    // PCM sits unconsumed.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint64_t after_withdraw = a->WindowsProcessedForTest();
    a->FeedPcm(pcm.data(), 2048, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(160));
    CHECK(a->WindowsProcessedForTest() == after_withdraw);
}
