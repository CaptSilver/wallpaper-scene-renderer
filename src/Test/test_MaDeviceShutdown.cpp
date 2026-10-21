#include <doctest.h>

#include "Audio/MaDeviceShutdown.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

// A custom miniaudio backend that behaves like the PulseAudio one where it
// matters: it has a data loop (so miniaudio treats it as synchronous), and
// that loop sleeps in the equivalent of pa_mainloop_iterate(block=1) until
// either onDeviceDataLoopWakeup is called or a server event arrives.  A
// capture stream on the monitor of an idle sink gets no server events, so
// the "event" here comes only after kServerEventAfter; it exists to let a
// shutdown that never wakes the loop finish, slowly, instead of hanging the
// suite.
namespace
{

constexpr auto kServerEventAfter = std::chrono::seconds(2);

struct FakeMainLoop {
    std::mutex              m;
    std::condition_variable cv;
    bool                    wakeupRequested { false };
    bool                    leftByWakeup { false };
    int                     serverEvents { 0 };
};

FakeMainLoop* loopOf(ma_device* d) { return static_cast<FakeMainLoop*>(d->pUserData); }

ma_result fakeDeviceInit(ma_device* pDevice, const ma_device_config*                pConfig,
                         ma_device_descriptor* /*pPlayback*/, ma_device_descriptor* pCapture) {
    if (pConfig->deviceType != ma_device_type_capture) return MA_DEVICE_TYPE_NOT_SUPPORTED;
    pCapture->format     = ma_format_f32;
    pCapture->channels   = 2;
    pCapture->sampleRate = 48000;
    ma_channel_map_init_standard(
        ma_standard_channel_map_default, pCapture->channelMap, MA_MAX_CHANNELS, pCapture->channels);
    pCapture->periodSizeInFrames = 480;
    pCapture->periodCount        = 2;
    (void)pDevice;
    return MA_SUCCESS;
}

ma_result fakeDeviceUninit(ma_device*) { return MA_SUCCESS; }
ma_result fakeDeviceStart(ma_device*) { return MA_SUCCESS; }
ma_result fakeDeviceStop(ma_device*) { return MA_SUCCESS; }

ma_result fakeDataLoop(ma_device* pDevice) {
    FakeMainLoop* loop = loopOf(pDevice);
    while (ma_device_get_state(pDevice) == ma_device_state_started) {
        std::unique_lock<std::mutex> lk(loop->m);
        if (loop->cv.wait_for(lk, kServerEventAfter, [&] {
                return loop->wakeupRequested;
            })) {
            loop->wakeupRequested = false;
            loop->leftByWakeup    = true;
        } else {
            loop->serverEvents++;
        }
    }
    return MA_SUCCESS;
}

ma_result fakeDataLoopWakeup(ma_device* pDevice) {
    FakeMainLoop* loop = loopOf(pDevice);
    {
        std::lock_guard<std::mutex> lk(loop->m);
        loop->wakeupRequested = true;
    }
    loop->cv.notify_all();
    return MA_SUCCESS;
}

ma_result fakeContextUninit(ma_context*) { return MA_SUCCESS; }

ma_result fakeContextInit(ma_context*, const ma_context_config*, ma_backend_callbacks* cb) {
    cb->onContextUninit        = fakeContextUninit;
    cb->onDeviceInit           = fakeDeviceInit;
    cb->onDeviceUninit         = fakeDeviceUninit;
    cb->onDeviceStart          = fakeDeviceStart;
    cb->onDeviceStop           = fakeDeviceStop;
    cb->onDeviceDataLoop       = fakeDataLoop;
    cb->onDeviceDataLoopWakeup = fakeDataLoopWakeup;
    return MA_SUCCESS;
}

struct FakeBackend {
    ma_context ctx {};
    FakeBackend() {
        ma_context_config cfg    = ma_context_config_init();
        cfg.custom.onContextInit = fakeContextInit;
        ma_backend backends[]    = { ma_backend_custom };
        REQUIRE(ma_context_init(backends, 1, &cfg, &ctx) == MA_SUCCESS);
    }
    ~FakeBackend() { ma_context_uninit(&ctx); }

    void open(ma_device& dev, FakeMainLoop& loop) {
        ma_device_config cfg = ma_device_config_init(ma_device_type_capture);
        cfg.capture.format   = ma_format_f32;
        cfg.capture.channels = 2;
        cfg.sampleRate       = 48000;
        cfg.pUserData        = &loop;
        REQUIRE(ma_device_init(&ctx, &cfg, &dev) == MA_SUCCESS);
    }
};

} // namespace

TEST_SUITE("StopAndUninitDevice") {
    TEST_CASE("a started device on a silent stream shuts down without waiting for audio") {
        FakeBackend  backend;
        FakeMainLoop loop;
        ma_device    dev {};
        backend.open(dev, loop);
        REQUIRE(ma_device_start(&dev) == MA_SUCCESS);

        const auto t0 = std::chrono::steady_clock::now();
        wallpaper::audio::StopAndUninitDevice(&dev);
        const auto elapsed = std::chrono::steady_clock::now() - t0;

        // ma_device_uninit alone leaves the loop asleep until the next server
        // event; on a real silent monitor that event never comes.
        CHECK(loop.leftByWakeup);
        CHECK(loop.serverEvents == 0);
        CHECK(elapsed < kServerEventAfter / 2);
        CHECK(ma_device_get_state(&dev) == ma_device_state_uninitialized);
    }

    TEST_CASE("a device that was never started is uninitialised without a stop") {
        FakeBackend  backend;
        FakeMainLoop loop;
        ma_device    dev {};
        backend.open(dev, loop);

        wallpaper::audio::StopAndUninitDevice(&dev);

        CHECK_FALSE(loop.leftByWakeup);
        CHECK(ma_device_get_state(&dev) == ma_device_state_uninitialized);
    }

    TEST_CASE("a zeroed device that was never initialised is left alone") {
        ma_device dev {};
        wallpaper::audio::StopAndUninitDevice(&dev);
        CHECK(ma_device_get_state(&dev) == ma_device_state_uninitialized);
    }
}
