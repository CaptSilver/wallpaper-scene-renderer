#include <doctest.h>

#include "Audio/AudioAnalyzer.h"

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace wallpaper::audio;

// AudioAnalyzer needs enough PCM in the SPSC ring buffer to fill FFT_SIZE (512)
// frames * 2 channels = 1024 samples before Process() will do any work.
constexpr uint32_t FFT_SIZE          = 512;
constexpr uint32_t MIN_FRAMES_TO_FFT = FFT_SIZE; // stereo frames

// Build an interleaved stereo sine wave at the given frequency and sample rate.
static std::vector<float> makeSineStereo(float freqHz, uint32_t frames, uint32_t sampleRate = 48000,
                                         float amplitude = 0.5f) {
    std::vector<float> pcm(frames * 2);
    for (uint32_t i = 0; i < frames; i++) {
        float s = amplitude * std::sin(2.0f * (float)M_PI * freqHz * (float)i / (float)sampleRate);
        pcm[i * 2]     = s;
        pcm[i * 2 + 1] = s;
    }
    return pcm;
}

TEST_SUITE("AudioAnalyzer.Basic") {
    TEST_CASE("fresh analyzer reports no data") {
        AudioAnalyzer a;
        CHECK_FALSE(a.HasData());
        CHECK(a.GetSpectrum16Left().size() == 16 * 4);
        CHECK(a.GetSpectrum16Right().size() == 16 * 4);
        CHECK(a.GetSpectrum32Left().size() == 32 * 4);
        CHECK(a.GetSpectrum64Left().size() == 64 * 4);
    }

    TEST_CASE("Process without data is a no-op (HasData stays false)") {
        AudioAnalyzer a;
        a.Process();
        CHECK_FALSE(a.HasData());
    }

    TEST_CASE("FeedPcm with frameCount=0 is a no-op (HasData stays false)") {
        // miniaudio's STARTED notification can fire FeedPcm with frameCount=0;
        // the function should early-return without touching writePos so
        // Process() still observes an empty ring.
        AudioAnalyzer a;
        float         dummy = 0.0f;
        a.FeedPcm(&dummy, /*frameCount=*/0, /*channels=*/2);
        a.Process();
        CHECK_FALSE(a.HasData());
    }

    TEST_CASE("Process below FFT_SIZE leaves HasData false") {
        AudioAnalyzer a;
        auto          pcm = makeSineStereo(440.f, FFT_SIZE - 1);
        a.FeedPcm(pcm.data(), FFT_SIZE - 1, 2);
        a.Process();
        CHECK_FALSE(a.HasData());
    }

    TEST_CASE("Process after enough PCM fills spectra and sets HasData") {
        AudioAnalyzer a;
        auto          pcm = makeSineStereo(1000.f, MIN_FRAMES_TO_FFT * 2);
        a.FeedPcm(pcm.data(), MIN_FRAMES_TO_FFT * 2, 2);
        a.Process();
        REQUIRE(a.HasData());

        // All 64 bands should have been written — none equals NaN, all in [0, ~1].
        auto raw64L = a.GetRawSpectrum(64, 0);
        REQUIRE(raw64L.size() == 64);
        for (float v : raw64L) {
            CHECK(std::isfinite(v));
            CHECK(v >= 0.0f);
            CHECK(v <= 1.0f);
        }
    }

} // Basic

TEST_SUITE("AudioAnalyzer.Spectrum") {
    TEST_CASE("padded spectrum has value at stride 0, zeros elsewhere") {
        AudioAnalyzer a;
        auto          pcm = makeSineStereo(500.f, MIN_FRAMES_TO_FFT * 2);
        a.FeedPcm(pcm.data(), MIN_FRAMES_TO_FFT * 2, 2);
        a.Process();
        auto pad16 = a.GetSpectrum16Left();
        REQUIRE(pad16.size() == 64);
        for (size_t i = 0; i < 16; i++) {
            // zeros at positions i*4+1, i*4+2, i*4+3
            CHECK(pad16[i * 4 + 1] == 0.0f);
            CHECK(pad16[i * 4 + 2] == 0.0f);
            CHECK(pad16[i * 4 + 3] == 0.0f);
        }
    }

    TEST_CASE("32-band spectrum is pairwise average of 64-band spectrum") {
        AudioAnalyzer a;
        auto          pcm = makeSineStereo(800.f, MIN_FRAMES_TO_FFT * 2);
        a.FeedPcm(pcm.data(), MIN_FRAMES_TO_FFT * 2, 2);
        a.Process();
        auto r64 = a.GetRawSpectrum(64, 0);
        auto r32 = a.GetRawSpectrum(32, 0);
        for (size_t b = 0; b < 32; b++) {
            float expected = (r64[b * 2] + r64[b * 2 + 1]) * 0.5f;
            CHECK(r32[b] == doctest::Approx(expected));
        }
    }

    TEST_CASE("16-band spectrum is pairwise average of 32-band spectrum") {
        AudioAnalyzer a;
        auto          pcm = makeSineStereo(600.f, MIN_FRAMES_TO_FFT * 2);
        a.FeedPcm(pcm.data(), MIN_FRAMES_TO_FFT * 2, 2);
        a.Process();
        auto r32 = a.GetRawSpectrum(32, 1);
        auto r16 = a.GetRawSpectrum(16, 1);
        for (size_t b = 0; b < 16; b++) {
            float expected = (r32[b * 2] + r32[b * 2 + 1]) * 0.5f;
            CHECK(r16[b] == doctest::Approx(expected));
        }
    }

    TEST_CASE("GetRawSpectrum with unsupported resolution returns empty span") {
        AudioAnalyzer a;
        CHECK(a.GetRawSpectrum(100, 0).size() == 0);
        CHECK(a.GetRawSpectrum(0, 0).size() == 0);
    }

    TEST_CASE("GetRawSpectrum selects channel correctly") {
        AudioAnalyzer      a;
        std::vector<float> pcm(MIN_FRAMES_TO_FFT * 2 * 2);
        // Left=loud sine, right=silent — then bands should differ.
        for (uint32_t i = 0; i < MIN_FRAMES_TO_FFT * 2; i++) {
            float s        = 0.8f * std::sin(2.0f * (float)M_PI * 2000.f * (float)i / 48000.f);
            pcm[i * 2]     = s;
            pcm[i * 2 + 1] = 0.0f;
        }
        a.FeedPcm(pcm.data(), MIN_FRAMES_TO_FFT * 2, 2);
        a.Process();

        auto l = a.GetRawSpectrum(64, 0);
        auto r = a.GetRawSpectrum(64, 1);
        // At least one band should show the left-channel energy without a matching
        // right-channel energy.
        bool asymmetryFound = false;
        for (size_t b = 0; b < 64; b++) {
            if (l[b] > r[b] + 0.05f) {
                asymmetryFound = true;
                break;
            }
        }
        CHECK(asymmetryFound);
    }

} // Spectrum

TEST_SUITE("AudioAnalyzer.Channels") {
    TEST_CASE("mono input (1 channel) duplicates to both L and R") {
        AudioAnalyzer a;
        // Mono interleaved: just frameCount samples
        std::vector<float> mono(MIN_FRAMES_TO_FFT * 2);
        for (uint32_t i = 0; i < MIN_FRAMES_TO_FFT * 2; i++) {
            mono[i] = 0.5f * std::sin(2.f * (float)M_PI * 440.f * (float)i / 48000.f);
        }
        a.FeedPcm(mono.data(), MIN_FRAMES_TO_FFT * 2, 1);
        a.Process();
        auto l = a.GetRawSpectrum(64, 0);
        auto r = a.GetRawSpectrum(64, 1);
        for (size_t b = 0; b < 64; b++) {
            CHECK(l[b] == doctest::Approx(r[b]));
        }
    }

    TEST_CASE(">2 channels: first 2 are kept") {
        AudioAnalyzer      a;
        const uint32_t     ch = 5;
        std::vector<float> pcm(MIN_FRAMES_TO_FFT * 2 * ch);
        for (uint32_t i = 0; i < MIN_FRAMES_TO_FFT * 2; i++) {
            pcm[i * ch + 0] = 0.3f; // L
            pcm[i * ch + 1] = 0.3f; // R
            // Other channels contain garbage that should be ignored
            for (uint32_t c = 2; c < ch; c++) pcm[i * ch + c] = 999.f;
        }
        a.FeedPcm(pcm.data(), MIN_FRAMES_TO_FFT * 2, ch);
        a.Process();
        REQUIRE(a.HasData());
        for (float v : a.GetRawSpectrum(64, 0)) CHECK(std::isfinite(v));
    }

} // Channels

TEST_SUITE("AudioAnalyzer.Smoothing") {
    TEST_CASE("silence after activity decays spectrum but does not go negative") {
        AudioAnalyzer a;
        auto          loud = makeSineStereo(1500.f, MIN_FRAMES_TO_FFT * 2, 48000, 0.8f);
        a.FeedPcm(loud.data(), MIN_FRAMES_TO_FFT * 2, 2);
        a.Process();

        std::vector<float> silence(MIN_FRAMES_TO_FFT * 2 * 2, 0.0f);
        // Feed enough silence to refill the ring with zeros, then Process.
        for (int i = 0; i < 10; i++) {
            a.FeedPcm(silence.data(), MIN_FRAMES_TO_FFT * 2, 2);
            a.Process();
        }
        for (float v : a.GetRawSpectrum(64, 0)) {
            CHECK(v >= 0.0f);
            CHECK(std::isfinite(v));
        }
    }

} // Smoothing

// MPSC producer race + consumer overrun cushion.  These tests pin the contract:
//   (1) two concurrent FeedPcm callers don't tear ring stores (verified under
//       --tsan, silent otherwise);
//   (2) the widened RING_SIZE tolerates a producer overshoot of >85ms before
//       the consumer's latest-N window wraps into in-flight writes;
//   (3) stall recovery picks the latest window, not a torn one;
//   (4) a callback toggle between producers leaves HasData true.
TEST_SUITE("AudioAnalyzer.ConcurrentProducers") {

    // (1) Race detector under --tsan.  Two threads call FeedPcm concurrently
    // with disjoint sine inputs while the main thread Process()es in a loop.
    // Under WEK_SANITIZE=thread this finding fires on the current (unmutexed)
    // FeedPcm because both threads race on ring[wp % RING_SIZE].  After the
    // mutex lands, tsan recognizes std::mutex as a sync primitive and the run
    // is clean.  Without tsan the test passes functionally — it joins cleanly
    // and the analyzer's HasData becomes true.
    TEST_CASE("two concurrent FeedPcm callers do not corrupt the ring (--tsan gated)") {
        AudioAnalyzer     a;
        std::atomic<bool> stop { false };
        // Each producer feeds its own buffer; the test does not care about
        // spectral content, only about ring-write integrity.
        constexpr uint32_t kFramesPerCall = 256;
        std::vector<float> bufA(kFramesPerCall * 2);
        std::vector<float> bufB(kFramesPerCall * 2);
        for (uint32_t i = 0; i < kFramesPerCall; ++i) {
            bufA[i * 2 + 0] = std::sin(0.05f * (float)i);
            bufA[i * 2 + 1] = std::sin(0.05f * (float)i + 0.5f);
            bufB[i * 2 + 0] = std::sin(0.10f * (float)i + 1.0f);
            bufB[i * 2 + 1] = std::sin(0.10f * (float)i + 1.5f);
        }
        std::thread tA([&] {
            for (int n = 0; n < 4000 && ! stop.load(); ++n)
                a.FeedPcm(bufA.data(), kFramesPerCall, 2);
        });
        std::thread tB([&] {
            for (int n = 0; n < 4000 && ! stop.load(); ++n)
                a.FeedPcm(bufB.data(), kFramesPerCall, 2);
        });
        // Drive Process() until either HasData becomes true or both producers
        // have finished.  The contract under test is "no ring corruption"; the
        // proxy assertion is that at least one Process() call observes enough
        // committed PCM to run an FFT.  A fixed iteration count is unreliable
        // because the consumer may race ahead of producers (especially with
        // the producer-side mutex slowing them) and finish before the first
        // committed write — so we synchronize on the contract end-state
        // (HasData) and on the producers' join.
        bool hasData = false;
        for (int n = 0; n < 100000 && ! hasData; ++n) {
            a.Process();
            hasData = a.HasData();
        }
        stop.store(true);
        tA.join();
        tB.join();
        // One last Process after the producers finished, to consume any
        // committed-but-not-yet-FFT'd tail.
        a.Process();
        CHECK(a.HasData()); // some Process() succeeded — functional check
    }

    // (2) Producer overrun cushion — confirms RING_SIZE was widened so the
    // consumer can fall ~250ms behind without reading torn samples.  At 48kHz
    // stereo, FFT_SIZE*2 = 1024 floats; RING_SIZE = 32768 leaves 31744 floats =
    // ~330ms of headroom.  Test: feed > 85ms but < 250ms, Process() once, the
    // spectrum reflects the LAST FFT_SIZE samples (a recognizable
    // single-frequency peak).
    TEST_CASE("widened RING_SIZE tolerates a ~150ms producer overshoot") {
        AudioAnalyzer a;
        // Feed 16384 stereo frames at 48kHz = ~341ms total; the last
        // FFT_SIZE = 512 frames are a clean 440Hz sine.  Under RING_SIZE 32768
        // this fits in less than one ring pass, so the consumer's latest-1024-
        // float window is genuinely the latter ~10.7ms of 440Hz, not a torn
        // mix.  (Under the OLD 8192 ring the producer would lap; the test
        // still passes since the consumer would happen to read whatever the
        // producer last wrote — but the assertion below pins behavior under
        // the WIDENED ring.)
        constexpr uint32_t kFrames = 16384;
        std::vector<float> pcm(kFrames * 2, 0.0f);
        // Latter half: 440Hz on both channels (lower frequency for stable
        // band-mapping).
        for (uint32_t i = kFrames / 2; i < kFrames; ++i) {
            float s = std::sin(2.0f * (float)M_PI * 440.0f * (float)i / 48000.0f);
            pcm[i * 2 + 0] = s;
            pcm[i * 2 + 1] = s;
        }
        a.FeedPcm(pcm.data(), kFrames, 2);
        a.Process();
        REQUIRE(a.HasData());
        auto spec = a.GetRawSpectrum(64, 0);
        REQUIRE(spec.size() == 64);
        // A 440Hz sine peaks in a low band (band index < 16 for 64-band
        // log-spaced mapping over [20Hz, 20kHz]).  Just assert the peak is in
        // the lower half — the precise band depends on the band mapping which
        // is not the contract under test here.
        float  maxVal = 0.0f;
        size_t maxIdx = 0;
        for (size_t i = 0; i < spec.size(); ++i)
            if (spec[i] > maxVal) {
                maxVal = spec[i];
                maxIdx = i;
            }
        CHECK(maxVal > 0.05f);
        CHECK(maxIdx < spec.size() / 2);
    }

    // (3) Stall recovery — feed → Process → "stall" (no Process) → feed a
    // different frequency → Process.  The second spectrum must reflect the
    // SECOND frequency only (no ghost peak at the first).  This is the
    // post-stall snapshot test: even with the producer racing past the
    // consumer during the stall, the consumer's wp - FFT_SIZE*2 anchor lands
    // on the LATEST writes.
    TEST_CASE("stall recovery picks the latest window, no ghost peak") {
        AudioAnalyzer      a;
        constexpr uint32_t kFrames = 4096;
        std::vector<float> pcm(kFrames * 2, 0.0f);
        // First: 880Hz sine.
        for (uint32_t i = 0; i < kFrames; ++i) {
            float s = std::sin(2.0f * (float)M_PI * 880.0f * (float)i / 48000.0f);
            pcm[i * 2 + 0] = s;
            pcm[i * 2 + 1] = s;
        }
        a.FeedPcm(pcm.data(), kFrames, 2);
        a.Process();
        REQUIRE(a.HasData());
        auto               specA = a.GetRawSpectrum(64, 0);
        std::vector<float> specA_copy(specA.begin(), specA.end());
        // "Stall": no Process call.  Now feed a clearly different frequency
        // (110Hz, well-separated band) for the same duration.
        for (uint32_t i = 0; i < kFrames; ++i) {
            float s = std::sin(2.0f * (float)M_PI * 110.0f * (float)i / 48000.0f);
            pcm[i * 2 + 0] = s;
            pcm[i * 2 + 1] = s;
        }
        a.FeedPcm(pcm.data(), kFrames, 2);
        a.Process();
        auto specB = a.GetRawSpectrum(64, 0);
        // The peak bands must differ: 880Hz's peak band > 110Hz's peak band by
        // enough that we can confirm the spectrum updated rather than
        // continuing to reflect the prior input.
        size_t maxA = 0, maxB = 0;
        float  vA = 0, vB = 0;
        for (size_t i = 0; i < specA_copy.size(); ++i)
            if (specA_copy[i] > vA) {
                vA   = specA_copy[i];
                maxA = i;
            }
        for (size_t i = 0; i < specB.size(); ++i)
            if (specB[i] > vB) {
                vB   = specB[i];
                maxB = i;
            }
        CHECK(maxA != maxB); // spectra are distinguishably different
        CHECK(maxB < maxA);  // 110Hz peaks lower than 880Hz
    }

    // (4) Toggle simulation — capture-mock feeds, then "toggle" (drop one
    // feeder), then BGM-mock feeds.  HasData must stay true across the toggle
    // (no spurious zero-spectrum frame caused by the producer swap).
    TEST_CASE("toggle between two feeders keeps HasData stable") {
        AudioAnalyzer      a;
        constexpr uint32_t kFrames = 1024;
        std::vector<float> pcm(kFrames * 2, 0.0f);
        for (uint32_t i = 0; i < kFrames; ++i) {
            float s = std::sin(2.0f * (float)M_PI * 220.0f * (float)i / 48000.0f);
            pcm[i * 2 + 0] = s;
            pcm[i * 2 + 1] = s;
        }
        a.FeedPcm(pcm.data(), kFrames, 2); // feeder #1 (capture)
        a.Process();
        CHECK(a.HasData());
        // "Toggle" — no analyzer reset; the second feeder takes over.
        a.FeedPcm(pcm.data(), kFrames, 2); // feeder #2 (BGM tap)
        a.Process();
        CHECK(a.HasData()); // still true post-toggle
    }

} // ConcurrentProducers
