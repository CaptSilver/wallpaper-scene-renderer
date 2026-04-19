#include <doctest.h>

#include "Audio/AudioAnalyzer.h"

#include <cmath>
#include <vector>

using namespace wallpaper::audio;

// AudioAnalyzer needs enough PCM in the SPSC ring buffer to fill FFT_SIZE (512)
// frames * 2 channels = 1024 samples before Process() will do any work.
constexpr uint32_t FFT_SIZE           = 512;
constexpr uint32_t MIN_FRAMES_TO_FFT  = FFT_SIZE; // stereo frames

// Build an interleaved stereo sine wave at the given frequency and sample rate.
static std::vector<float> makeSineStereo(float freqHz, uint32_t frames,
                                         uint32_t sampleRate = 48000,
                                         float amplitude = 0.5f) {
    std::vector<float> pcm(frames * 2);
    for (uint32_t i = 0; i < frames; i++) {
        float s     = amplitude * std::sin(2.0f * (float)M_PI * freqHz * (float)i / (float)sampleRate);
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
        float s     = 0.8f * std::sin(2.0f * (float)M_PI * 2000.f * (float)i / 48000.f);
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
