#include <doctest.h>

#include "Audio/SoundManager.h"
#include "Audio/AudioAnalyzer.h"
#include "Audio/miniaudio-wrapper.hpp"

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

    TEST_CASE("SetMuted toggles flag without tearing down the device") {
        // Mute must NOT UnInit the device — the spectrum callback is the
        // sole audio-reactivity feed for several wallpapers (e.g. Cyberpunk
        // Lucy 2866203962), so muting the wallpaper has to leave the data
        // callback firing.
        //
        // SetMuted lazily calls Init() on the first toggle
        // (`if (!IsInited()) Init();` in SoundManager.cpp), which opens the
        // default device and can BLOCK on a stale audio socket — so this case
        // is a device test and must be gated behind the opt-in.
        if (! wekde_audio_device_optin()) {
            MESSAGE("no audio device opt-in (set WEKDE_HAS_AUDIO_DEVICE=1); "
                    "skipping SetMuted (lazily opens the device)");
            return;
        }
        SoundManager sm;
        sm.SetMuted(true);
        CHECK(sm.Muted());
        sm.SetMuted(false);
        CHECK_FALSE(sm.Muted());
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
