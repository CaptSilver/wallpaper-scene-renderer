#include "Audio/AudioAnalyzer.h"
#include "Utils/Logging.h"

#include <kiss_fft.h>
#include <kiss_fftr.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

using namespace wallpaper::audio;

namespace
{
constexpr uint32_t FFT_SIZE       = 512;  // power of 2, ~10.7ms at 48kHz
constexpr uint32_t NUM_BINS       = FFT_SIZE / 2 + 1; // 257
constexpr uint32_t NUM_BANDS_64   = 64;
constexpr uint32_t NUM_BANDS_32   = 32;
constexpr uint32_t NUM_BANDS_16   = 16;
constexpr uint32_t RING_SIZE      = 8192; // ~85ms at 48kHz stereo
constexpr float    SMOOTHING_ATTACK = 0.8f;
constexpr float    SMOOTHING_DECAY  = 0.4f;
constexpr float    MIN_FREQ       = 20.0f;
constexpr float    MAX_FREQ       = 20000.0f;
// Gain applied to raw FFT magnitudes to bring them into a useful [0,1] range.
// Raw FFT gives ~0.01-0.05 for typical music; WE expects ~0.3-0.8 peaks.
// We previously used 15.0 with a hard min()-clamp to 1.0, which saturated on
// louder tracks and pinned bands at 1.0.  Wallpaper 2866203962 stacks two
// chromatic_aberration passes on its VHS Time/Date text; with saturated bands
// the audio-reactive shift pushed the RGB channels so far apart that the
// glyphs looked corrupted / disappeared.  Dropping gain + using a smooth
// saturation keeps peaks around 0.3–0.6 on louder music (never hits 1.0) so
// audio-reactive effects stay musical without shredding text layers.
constexpr float    SPECTRUM_GAIN  = 4.0f;

// Precompute Hanning window coefficients
struct HanningWindow {
    std::array<float, FFT_SIZE> w;
    HanningWindow() {
        for (uint32_t i = 0; i < FFT_SIZE; i++) {
            w[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * (float)i / (float)(FFT_SIZE - 1)));
        }
    }
};
static const HanningWindow g_hanning;

// Map FFT bins to log-spaced bands
struct BandMapping {
    // For each of 64 bands: [start_bin, end_bin) — inclusive start, exclusive end
    std::array<uint32_t, NUM_BANDS_64 + 1> edges;

    void Compute(uint32_t sampleRate) {
        float freqPerBin = (float)sampleRate / (float)FFT_SIZE;
        float logMin     = std::log10(MIN_FREQ);
        float logMax     = std::log10(MAX_FREQ);
        for (uint32_t b = 0; b <= NUM_BANDS_64; b++) {
            float logFreq = logMin + (logMax - logMin) * (float)b / (float)NUM_BANDS_64;
            float freq    = std::pow(10.0f, logFreq);
            uint32_t bin  = (uint32_t)(freq / freqPerBin + 0.5f);
            if (bin >= NUM_BINS) bin = NUM_BINS - 1;
            edges[b] = bin;
        }
        // Ensure each band covers at least one bin
        for (uint32_t b = 0; b < NUM_BANDS_64; b++) {
            if (edges[b + 1] <= edges[b]) {
                edges[b + 1] = edges[b] + 1;
                if (edges[b + 1] >= NUM_BINS) edges[b + 1] = NUM_BINS - 1;
            }
        }
    }
};

} // namespace

struct AudioAnalyzer::Impl {
    // SPSC ring buffer for lock-free PCM transfer from audio thread
    std::array<float, RING_SIZE> ring {};
    std::atomic<uint32_t>        writePos { 0 };
    uint32_t                     readPos { 0 };

    // FFT state
    kiss_fftr_cfg fftCfg { nullptr };
    std::array<float, FFT_SIZE>       windowedL {};
    std::array<float, FFT_SIZE>       windowedR {};
    std::array<kiss_fft_cpx, NUM_BINS> freqL {};
    std::array<kiss_fft_cpx, NUM_BINS> freqR {};
    std::array<float, NUM_BINS>       magL {};
    std::array<float, NUM_BINS>       magR {};

    // Band mapping
    BandMapping bandMap {};
    uint32_t    sampleRate { 48000 };

    // Raw spectrum (unpadded, for SceneScript)
    std::array<float, NUM_BANDS_64> rawL64 {};
    std::array<float, NUM_BANDS_64> rawR64 {};
    std::array<float, NUM_BANDS_32> rawL32 {};
    std::array<float, NUM_BANDS_32> rawR32 {};
    std::array<float, NUM_BANDS_16> rawL16 {};
    std::array<float, NUM_BANDS_16> rawR16 {};

    // Smoothed spectrum (unpadded)
    std::array<float, NUM_BANDS_64> smoothL64 {};
    std::array<float, NUM_BANDS_64> smoothR64 {};

    // std140-padded output: each float at vec4 stride (4 floats per element)
    std::array<float, NUM_BANDS_16 * 4> padL16 {};
    std::array<float, NUM_BANDS_16 * 4> padR16 {};
    std::array<float, NUM_BANDS_32 * 4> padL32 {};
    std::array<float, NUM_BANDS_32 * 4> padR32 {};
    std::array<float, NUM_BANDS_64 * 4> padL64 {};
    std::array<float, NUM_BANDS_64 * 4> padR64 {};

    bool hasData { false };

    Impl() {
        fftCfg = kiss_fftr_alloc((int)FFT_SIZE, 0, nullptr, nullptr);
        bandMap.Compute(sampleRate);
    }
    ~Impl() {
        if (fftCfg) kiss_fftr_free(fftCfg);
    }

    // Write std140-padded output: value at [i*4], zeros at [i*4+1..3]
    static void PadSpectrum(const float* src, float* dst, uint32_t count) {
        std::memset(dst, 0, count * 4 * sizeof(float));
        for (uint32_t i = 0; i < count; i++) {
            dst[i * 4] = src[i];
        }
    }
};

AudioAnalyzer::AudioAnalyzer(): m_impl(std::make_unique<Impl>()) {}
AudioAnalyzer::~AudioAnalyzer() = default;

void AudioAnalyzer::FeedPcm(const float* interleavedStereo, uint32_t frameCount,
                            uint32_t channels) {
    // Write interleaved stereo samples into ring buffer (SPSC: single producer)
    // If mono, duplicate to L+R; if >2 channels, take first 2
    uint32_t wp = m_impl->writePos.load(std::memory_order_relaxed);
    for (uint32_t f = 0; f < frameCount; f++) {
        float l, r;
        if (channels >= 2) {
            l = interleavedStereo[f * channels];
            r = interleavedStereo[f * channels + 1];
        } else {
            l = r = interleavedStereo[f * channels];
        }
        m_impl->ring[wp % RING_SIZE]       = l;
        m_impl->ring[(wp + 1) % RING_SIZE] = r;
        wp += 2;
    }
    m_impl->writePos.store(wp, std::memory_order_release);
}

void AudioAnalyzer::Process() {
    auto& d = *m_impl;

    // Read latest FFT_SIZE stereo frames from ring buffer
    uint32_t wp = d.writePos.load(std::memory_order_acquire);
    uint32_t rp = d.readPos;

    uint32_t available = wp - rp; // wraps correctly for unsigned
    if (available < FFT_SIZE * 2) {
        // Not enough data yet
        return;
    }

    // Skip to latest FFT_SIZE frames worth of stereo samples
    if (available > FFT_SIZE * 2) {
        rp = wp - FFT_SIZE * 2;
    }

    // Deinterleave + apply Hanning window
    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        uint32_t idx = (rp + i * 2) % RING_SIZE;
        d.windowedL[i] = d.ring[idx] * g_hanning.w[i];
        d.windowedR[i] = d.ring[(idx + 1) % RING_SIZE] * g_hanning.w[i];
    }
    d.readPos = wp;

    // FFT
    kiss_fftr(d.fftCfg, d.windowedL.data(), d.freqL.data());
    kiss_fftr(d.fftCfg, d.windowedR.data(), d.freqR.data());

    // Magnitude
    float norm = 2.0f / (float)FFT_SIZE;
    for (uint32_t k = 0; k < NUM_BINS; k++) {
        float reL = d.freqL[k].r, imL = d.freqL[k].i;
        float reR = d.freqR[k].r, imR = d.freqR[k].i;
        d.magL[k] = std::sqrt(reL * reL + imL * imL) * norm;
        d.magR[k] = std::sqrt(reR * reR + imR * imR) * norm;
    }

    // Map bins to 64 log-spaced bands
    for (uint32_t b = 0; b < NUM_BANDS_64; b++) {
        uint32_t lo = d.bandMap.edges[b];
        uint32_t hi = d.bandMap.edges[b + 1];
        if (hi <= lo) hi = lo + 1;
        if (hi > NUM_BINS) hi = NUM_BINS;
        float sumL = 0, sumR = 0;
        for (uint32_t k = lo; k < hi; k++) {
            sumL += d.magL[k];
            sumR += d.magR[k];
        }
        float n    = (float)(hi - lo);
        // Gain + soft saturate for perceptual scaling: raw FFT magnitudes are
        // small (0.01-0.05 for typical music); apply gain and sqrt for
        // perceptual loudness.  We then pass through x/(1+x) for a smooth
        // asymptote ~1.0 — never hits hard 1.0 so stacked audio-reactive
        // effects (see chromatic_aberration comment above) don't clamp.
        auto softSat = [](float x) {
            x = std::sqrt(std::max(x, 0.0f));
            return x / (1.0f + x * 0.4f);
        };
        float newL = softSat(sumL / n * SPECTRUM_GAIN);
        float newR = softSat(sumR / n * SPECTRUM_GAIN);

        // Exponential smoothing: fast attack, slow decay
        if (newL >= d.smoothL64[b])
            d.smoothL64[b] = newL * SMOOTHING_ATTACK + d.smoothL64[b] * (1.0f - SMOOTHING_ATTACK);
        else
            d.smoothL64[b] = newL * (1.0f - SMOOTHING_DECAY) + d.smoothL64[b] * SMOOTHING_DECAY;

        if (newR >= d.smoothR64[b])
            d.smoothR64[b] = newR * SMOOTHING_ATTACK + d.smoothR64[b] * (1.0f - SMOOTHING_ATTACK);
        else
            d.smoothR64[b] = newR * (1.0f - SMOOTHING_DECAY) + d.smoothR64[b] * SMOOTHING_DECAY;

        d.rawL64[b] = d.smoothL64[b];
        d.rawR64[b] = d.smoothR64[b];
    }

    // 32 bands = pairwise average of 64
    for (uint32_t b = 0; b < NUM_BANDS_32; b++) {
        d.rawL32[b] = (d.rawL64[b * 2] + d.rawL64[b * 2 + 1]) * 0.5f;
        d.rawR32[b] = (d.rawR64[b * 2] + d.rawR64[b * 2 + 1]) * 0.5f;
    }

    // 16 bands = pairwise average of 32
    for (uint32_t b = 0; b < NUM_BANDS_16; b++) {
        d.rawL16[b] = (d.rawL32[b * 2] + d.rawL32[b * 2 + 1]) * 0.5f;
        d.rawR16[b] = (d.rawR32[b * 2] + d.rawR32[b * 2 + 1]) * 0.5f;
    }

    // Generate std140-padded output
    Impl::PadSpectrum(d.rawL16.data(), d.padL16.data(), NUM_BANDS_16);
    Impl::PadSpectrum(d.rawR16.data(), d.padR16.data(), NUM_BANDS_16);
    Impl::PadSpectrum(d.rawL32.data(), d.padL32.data(), NUM_BANDS_32);
    Impl::PadSpectrum(d.rawR32.data(), d.padR32.data(), NUM_BANDS_32);
    Impl::PadSpectrum(d.rawL64.data(), d.padL64.data(), NUM_BANDS_64);
    Impl::PadSpectrum(d.rawR64.data(), d.padR64.data(), NUM_BANDS_64);

    d.hasData = true;
}

std::span<const float> AudioAnalyzer::GetSpectrum16Left() const {
    return m_impl->padL16;
}
std::span<const float> AudioAnalyzer::GetSpectrum16Right() const {
    return m_impl->padR16;
}
std::span<const float> AudioAnalyzer::GetSpectrum32Left() const {
    return m_impl->padL32;
}
std::span<const float> AudioAnalyzer::GetSpectrum32Right() const {
    return m_impl->padR32;
}
std::span<const float> AudioAnalyzer::GetSpectrum64Left() const {
    return m_impl->padL64;
}
std::span<const float> AudioAnalyzer::GetSpectrum64Right() const {
    return m_impl->padR64;
}

std::span<const float> AudioAnalyzer::GetRawSpectrum(int resolution, int channel) const {
    auto& d = *m_impl;
    // channel: 0=left, 1=right
    switch (resolution) {
    case 16: return channel == 0 ? std::span<const float>(d.rawL16) : std::span<const float>(d.rawR16);
    case 32: return channel == 0 ? std::span<const float>(d.rawL32) : std::span<const float>(d.rawR32);
    case 64: return channel == 0 ? std::span<const float>(d.rawL64) : std::span<const float>(d.rawR64);
    default: return {};
    }
}

bool AudioAnalyzer::HasData() const {
    return m_impl->hasData;
}
