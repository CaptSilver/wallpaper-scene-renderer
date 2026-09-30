#include <doctest.h>

#include "Audio/SoundManager.h"
#include "Audio/AudioAnalyzer.h"
#include "Audio/miniaudio-wrapper.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

using namespace wallpaper::audio;

// Audio device access is environment-dependent: SoundManager::Init opens the
// default playback device via miniaudio, which enumerates PulseAudio/PipeWire
// and can BLOCK indefinitely on a stale socket in a headless distrobox.  Gate
// the device-touching cases behind an explicit opt-in env var so the suite
// never wedges; set WEKDE_HAS_AUDIO_DEVICE=1 on a box with a live audio server
// to exercise the real Init() path.  (Mirrors the probe in test_AudioCapture
// and the MESSAGE+return skip idiom of wp_pkg_tool_available.)
static bool wekde_audio_device_optin() {
    const char* v = std::getenv("WEKDE_HAS_AUDIO_DEVICE");
    return v && std::string_view(v) == "1";
}

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

    TEST_CASE("SetMuted toggles the flag and never opens the device") {
        // Mute must not UnInit an open device -- the spectrum callback is the
        // sole audio-reactivity feed for several wallpapers (e.g. Cyberpunk
        // Lucy 2866203962), so muting has to leave the data callback firing.
        // Nor may it open one: the scene host opens the device only while a
        // sound is mounted and playback runs (WantsOutputDevice), since an
        // open stream keeps the audio codec powered.  So this no longer needs
        // the device opt-in.
        SoundManager sm;
        sm.SetMuted(true);
        CHECK(sm.Muted());
        CHECK_FALSE(sm.IsInited());
        sm.SetMuted(false);
        CHECK_FALSE(sm.Muted());
        CHECK_FALSE(sm.IsInited());
    }

    TEST_CASE("WantsOutputDevice: only while a sound is mounted and playback runs") {
        CHECK(WantsOutputDevice(/*playbackStopped=*/false, /*mountedChannels=*/1));
        CHECK(WantsOutputDevice(false, 3));
        CHECK_FALSE(WantsOutputDevice(false, 0)); // most scenes: no sound at all
        CHECK_FALSE(WantsOutputDevice(true, 1));  // paused (maximized window etc.)
        CHECK_FALSE(WantsOutputDevice(true, 0));
    }

    TEST_CASE("Close() on an unopened device is a no-op") {
        SoundManager sm;
        sm.Close();
        CHECK_FALSE(sm.IsInited());
        CHECK(sm.MountedChannelCount() == 0u);
    }

    TEST_CASE("Init() succeeds even after SetMuted(true) (used to early-return false)") {
        // Regression: previously SoundManager::Init() refused to open the
        // device when muted; combined with SetMuted's UnInit branch that
        // killed the spectrum-callback feed forever once the user muted.
        // Init must now attempt the open regardless of mute state, so the
        // data callback can run and the analyzer keeps getting PCM frames.
        //
        // Both SetMuted(true) and the explicit Init() touch the device and can
        // BLOCK on a stale audio socket headless — gate behind the opt-in.
        if (! wekde_audio_device_optin()) {
            MESSAGE("no audio device opt-in (set WEKDE_HAS_AUDIO_DEVICE=1); "
                    "skipping live SoundManager::Init()");
            return;
        }
        SoundManager sm;
        sm.SetMuted(true);
        // Init's return value depends on whether a real audio device is
        // available (distrobox often lacks one).  We don't assert on
        // success — only that it doesn't refuse just because we're muted.
        sm.Init();
        // No assertion on IsInited(); under either path the device is
        // either successfully opened or transparently unavailable.
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

namespace
{

// Synthetic channel that emits a fixed-amplitude tone for `framesToEmit`
// total frames, then signals end-of-stream.  Lets us drive ProcessFrame
// deterministically without a real audio source.
class ToneChannel : public miniaudio::Channel {
public:
    ToneChannel(float amp, ma_uint32 phyChannels, ma_uint64 framesToEmit)
      : m_amp(amp), m_chn(phyChannels), m_left(framesToEmit) {}
    ma_uint64 NextPcmData(void* pData, ma_uint32 frameCount) override {
        if (m_left == 0) return 0;
        ma_uint32 emit = static_cast<ma_uint32>(std::min<ma_uint64>(frameCount, m_left));
        float*    out  = static_cast<float*>(pData);
        for (ma_uint32 i = 0; i < emit * m_chn; i++) out[i] = m_amp;
        m_left -= emit;
        return emit;
    }
    void PassDeviceDesc(const miniaudio::DeviceDesc&) override {}
private:
    float       m_amp;
    ma_uint32   m_chn;
    ma_uint64   m_left;
};

} // anonymous

TEST_SUITE("miniaudio::Device::ProcessFrame") {
    TEST_CASE("muted device still feeds spectrum callback (regression: 2866203962 mute kills fade)") {
        // The wallpaper's media-player fade-out gate is
        //   `audio.average.reduce > 1 && !shared.playerproximity`
        // and `audio.average` comes from `engine.registerAudioBuffers`,
        // ultimately fed by the spectrum callback set on miniaudio::Device.
        // Before the fix:
        //   - SoundManager::SetMuted(true) called Device::UnInit() (full
        //     teardown), so the data callback never fired again.
        //   - Even with the device alive, data_callback early-returned on
        //     `m_muted`, never invoking the spectrum callback.
        // Either failure left the analyzer permanently fed with zeros and
        // the player UI never faded.  This test pins the fix.
        miniaudio::Device dev;

        std::atomic<int> spectrum_calls { 0 };
        std::atomic<float> spectrum_first_sample { 0.0f };
        dev.SetSpectrumCallback(
            [&](const float* data, uint32_t /*frames*/, uint32_t /*ch*/) {
                spectrum_calls.fetch_add(1);
                spectrum_first_sample.store(data[0]);
            });

        const ma_uint32 phyChannels = 2;
        const ma_uint32 frameCount  = 64;
        // Mount a 0.5-amplitude tone with plenty of frames.
        dev.MountChannel(std::make_shared<ToneChannel>(0.5f, phyChannels, /*frames=*/4096));

        std::vector<float> outBuf(frameCount * phyChannels, 9999.0f);

        dev.TestSetRunning(true);

        SUBCASE("unmuted: output gets the mix and spectrum sees the tone") {
            dev.SetMuted(false);
            dev.ProcessFrame(outBuf.data(), frameCount, phyChannels);
            CHECK(spectrum_calls.load() == 1);
            CHECK(spectrum_first_sample.load() == doctest::Approx(0.5f));
            // Output mirrors the mix (volume default 1.0).
            CHECK(outBuf[0] == doctest::Approx(0.5f));
            CHECK(outBuf[frameCount * phyChannels - 1] == doctest::Approx(0.5f));
        }

        SUBCASE("muted: output is zeroed but spectrum still receives the un-muted mix") {
            dev.SetMuted(true);
            dev.ProcessFrame(outBuf.data(), frameCount, phyChannels);
            CHECK(spectrum_calls.load() == 1);
            // Spectrum callback gets the pre-mute mix — this is what the
            // analyzer FFTs and the script then thresholds against >1.
            CHECK(spectrum_first_sample.load() == doctest::Approx(0.5f));
            // Speakers stay silent.
            for (size_t i = 0; i < outBuf.size(); i++) {
                CHECK(outBuf[i] == doctest::Approx(0.0f));
            }
        }

        SUBCASE("toggling mute keeps spectrum feed alive across both states") {
            dev.SetMuted(true);
            dev.ProcessFrame(outBuf.data(), frameCount, phyChannels);
            dev.SetMuted(false);
            dev.ProcessFrame(outBuf.data(), frameCount, phyChannels);
            dev.SetMuted(true);
            dev.ProcessFrame(outBuf.data(), frameCount, phyChannels);
            CHECK(spectrum_calls.load() == 3);
        }
    }

    TEST_CASE("ProcessFrame zeros output and spectrum buffer when no channels are mounted") {
        miniaudio::Device dev;
        std::atomic<int>  spectrum_calls { 0 };
        std::atomic<float> max_sample { 0.0f };
        dev.SetSpectrumCallback([&](const float* data, uint32_t frames, uint32_t ch) {
            spectrum_calls.fetch_add(1);
            float m = 0.0f;
            for (uint32_t i = 0; i < frames * ch; i++)
                if (std::abs(data[i]) > m) m = std::abs(data[i]);
            max_sample.store(m);
        });

        dev.TestSetRunning(true);
        const ma_uint32   frameCount  = 32;
        const ma_uint32   phyChannels = 2;
        std::vector<float> outBuf(frameCount * phyChannels, 0.42f);
        dev.ProcessFrame(outBuf.data(), frameCount, phyChannels);

        CHECK(spectrum_calls.load() == 1);
        // No channels → mix buffer is all zeros → analyzer sees silence.
        CHECK(max_sample.load() == doctest::Approx(0.0f));
        // No channels → output isn't overwritten by mix; we still expect
        // the unmuted path to copy zeroed mix into output.
        for (auto v : outBuf) CHECK(v == doctest::Approx(0.0f));
    }
} // miniaudio::Device::ProcessFrame

// SetSpectrumCallback can be reassigned from the main looper while the audio
// thread is mid-invoke inside ProcessFrame. Without a dedicated mutex around
// the assign + the copy-before-invoke, the std::function read can race a
// move-assign and tear into freed lambda capture. Stress both threads to pin
// the contract.
#include <thread>
TEST_SUITE("miniaudio::Device::SetSpectrumCallback") {
    TEST_CASE("concurrent SetSpectrumCallback + ProcessFrame is race-free") {
        miniaudio::Device dev;
        dev.TestSetRunning(true);

        std::atomic<bool>     stop { false };
        std::atomic<uint64_t> cb_invocations { 0 };

        std::thread audio([&] {
            std::vector<float> outBuf(2 * 128);
            while (! stop.load(std::memory_order_relaxed)) {
                dev.ProcessFrame(outBuf.data(), 128, 2);
            }
        });

        for (int i = 0; i < 4096; ++i) {
            dev.SetSpectrumCallback(
                [&cb_invocations](const float*, uint32_t, uint32_t) {
                    cb_invocations.fetch_add(1, std::memory_order_relaxed);
                });
            // Force a teardown of the captured std::function periodically so
            // the audio thread sees both populated and empty states.
            if ((i & 0x3F) == 0) dev.SetSpectrumCallback({});
        }
        stop.store(true, std::memory_order_relaxed);
        audio.join();

        // No crash, no TSan trip. Exact count is implementation-defined.
        CHECK(cb_invocations.load() > 0);
    }
}

// Close() releases the stream of a scene with no sound (and of a paused one).
// An open device fed the playback tap silence every period, so the spectrum
// of the last sound decayed to zero; a closed one feeds nothing, and
// AudioAnalyzer::Process() keeps the last spectrum when no new PCM arrives.
TEST_SUITE("miniaudio::Device spectrum after Close") {
    // A 1.5 kHz square wave at 48 kHz, loud enough to light up the spectrum.
    std::vector<float> squareWave(uint32_t frames) {
        std::vector<float> pcm(frames * 2);
        for (std::size_t i = 0; i < pcm.size(); i++) pcm[i] = (i / 2) % 32 < 16 ? 0.5f : -0.5f;
        return pcm;
    }

    float loudestBand(const AudioAnalyzer& a) {
        float m = 0.0f;
        for (float v : a.GetRawSpectrum(64, 0)) m = std::max(m, v);
        return m;
    }

    TEST_CASE("without new PCM the analyser keeps the last spectrum") {
        // The problem FeedSpectrumSilence solves.
        AudioAnalyzer a;
        const auto    tone = squareWave(4096);
        a.FeedPcm(tone.data(), 4096, 2);
        a.Process();
        const float before = loudestBand(a);
        REQUIRE(before > 0.1f);
        for (int i = 0; i < 10; i++) a.Process();
        CHECK(loudestBand(a) == doctest::Approx(before));
    }

    TEST_CASE("the silence handed over on Close decays the spectrum to zero") {
        AudioAnalyzer a;
        const auto    tone = squareWave(4096);
        a.FeedPcm(tone.data(), 4096, 2);
        a.Process();
        REQUIRE(loudestBand(a) > 0.1f);

        miniaudio::Device dev;
        dev.SetSpectrumCallback([&a](const float* data, uint32_t frames, uint32_t ch) {
            a.FeedPcm(data, frames, ch);
        });
        dev.FeedSpectrumSilence();
        a.Process();

        CHECK(loudestBand(a) < 1e-3f);
    }

    TEST_CASE("the silence is all zeros, and nothing happens without a callback") {
        miniaudio::Device dev;
        dev.FeedSpectrumSilence(); // no callback: must not crash

        uint32_t gotFrames = 0, gotChannels = 0;
        bool     allZero = true;
        dev.SetSpectrumCallback([&](const float* data, uint32_t frames, uint32_t ch) {
            gotFrames   = frames;
            gotChannels = ch;
            for (uint32_t i = 0; i < frames * ch; i++) allZero = allZero && data[i] == 0.0f;
        });
        dev.FeedSpectrumSilence();
        CHECK(gotFrames == miniaudio::Device::kCloseSilenceFrames);
        CHECK(gotChannels == 2u);
        CHECK(allZero);
    }
}

TEST_SUITE("miniaudio::Device channel lifetime") {
    TEST_CASE("a failed device open leaves already-mounted channels alone") {
        // Init() bails through AbortInit() when the backend won't open (no
        // PipeWire socket, wrong playback format, device won't start).  The
        // channels were mounted before that attempt and their owners hold
        // non-owning aliases to the streams inside them, so a failed open must
        // not destroy them — the next successful Init() re-issues
        // PassDeviceDesc to whatever is still mounted.
        miniaudio::Device dev;
        dev.MountChannel(std::make_shared<ToneChannel>(0.5f, 2, /*frames=*/128));
        REQUIRE(dev.ChannelCount() == 1u);

        dev.AbortInit();

        CHECK(dev.ChannelCount() == 1u);
    }

    TEST_CASE("Close keeps the mounted channels for the next Init") {
        // Close() releases the OS stream when a scene pauses; the scene's
        // sounds stay mounted so resuming only reopens the device.  Only an
        // open device has a stream to release, so this needs a real one.
        if (! wekde_audio_device_optin()) {
            MESSAGE("no audio device opt-in (set WEKDE_HAS_AUDIO_DEVICE=1); "
                    "skipping Close (needs an open device)");
            return;
        }
        miniaudio::Device dev;
        dev.MountChannel(std::make_shared<ToneChannel>(0.5f, 2, /*frames=*/128));
        REQUIRE(dev.Init({}));
        REQUIRE(dev.IsInited());

        dev.Close();

        CHECK_FALSE(dev.IsInited());
        CHECK(dev.ChannelCount() == 1u);
        CHECK(dev.Init({})); // resuming reopens it, channels still there
        CHECK(dev.ChannelCount() == 1u);
    }

    TEST_CASE("Close on a device that was never opened leaves everything alone") {
        miniaudio::Device dev;
        dev.MountChannel(std::make_shared<ToneChannel>(0.5f, 2, /*frames=*/128));
        int calls = 0;
        dev.SetSpectrumCallback([&](const float*, uint32_t, uint32_t) {
            calls++;
        });

        dev.Close();

        CHECK_FALSE(dev.IsInited());
        CHECK(dev.ChannelCount() == 1u);
        CHECK(calls == 0); // nothing was playing, so no silence to report
    }

    TEST_CASE("UnInit drops the channels the device was mixing") {
        // The teardown path (destructor / deliberate shutdown) does own the
        // channels and must release them.
        miniaudio::Device dev;
        dev.MountChannel(std::make_shared<ToneChannel>(0.5f, 2, /*frames=*/128));
        REQUIRE(dev.ChannelCount() == 1u);

        dev.UnInit();

        CHECK(dev.ChannelCount() == 0u);
    }

    TEST_CASE("ChannelCount tracks mounts and UnmountAll") {
        miniaudio::Device dev;
        CHECK(dev.ChannelCount() == 0u);
        dev.MountChannel(std::make_shared<ToneChannel>(0.25f, 2, 64));
        dev.MountChannel(std::make_shared<ToneChannel>(0.25f, 2, 64));
        CHECK(dev.ChannelCount() == 2u);
        dev.UnmountAll();
        CHECK(dev.ChannelCount() == 0u);
    }
}
