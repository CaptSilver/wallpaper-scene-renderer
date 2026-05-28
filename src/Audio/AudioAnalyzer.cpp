#include "Audio/AudioAnalyzer.h"
#include "Utils/Logging.h"
#include "Utils/SceneProfiler.h"

#include <kiss_fft.h>
#include <kiss_fftr.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

using namespace wallpaper::audio;

namespace
{
constexpr uint32_t FFT_SIZE         = 512;              // power of 2, ~10.7ms at 48kHz
constexpr uint32_t NUM_BINS         = FFT_SIZE / 2 + 1; // 257
constexpr uint32_t NUM_BANDS_64     = 64;
constexpr uint32_t NUM_BANDS_32     = 32;
constexpr uint32_t NUM_BANDS_16     = 16;
// RING_SIZE: floats held in the producer→consumer ring.  Sized so the
// consumer (one Process per render frame, reads the latest FFT_SIZE*2 = 1024
// floats) has comfortable headroom against the producer's ~10ms miniaudio
// ticks before the producer laps the consumer's read window.  Must remain a
// power of two so wp % RING_SIZE lowers to an AND with RING_SIZE - 1.
//   8192 floats ≈ 4096 stereo frames ≈ 85ms at 48kHz — too tight for any
// stall over ~75ms (compositor hiccup, TTY-switch, suspend wake).  Widened to
// 32768 floats ≈ 16384 stereo frames ≈ 340ms at 48kHz; the consumer can fall
// up to ~330ms behind before reading torn samples.
constexpr uint32_t RING_SIZE        = 32768; // ~340ms at 48kHz stereo
constexpr float    SMOOTHING_ATTACK = 0.8f;
constexpr float    SMOOTHING_DECAY  = 0.4f;
constexpr float    MIN_FREQ         = 20.0f;
constexpr float    MAX_FREQ         = 20000.0f;
// Gain applied to raw FFT magnitudes to bring them into a useful [0,1] range.
// Raw FFT gives ~0.01-0.05 for typical music; WE expects ~0.3-0.8 peaks.
// History:
//   - 15.0 with hard min()-clamp to 1.0 — saturated on louder tracks and
//     pinned bands at 1.0; broke wallpaper 2866203962's stacked chromatic-
//     aberration text shake (RGB channels separated until glyphs vanished).
//   - 4.0 with soft saturation x/(1+0.4x) — peaks ~0.3–0.6, kept text
//     intact but audio visualizers (Blue Archive 2764537029, similar) felt
//     visibly under-pumped: bars topped out at ~30px instead of the
//     ~60–80px the wallpapers visually aim for.
//   - 8.0 with the same soft saturation — peaks land around 0.5–0.7,
//     visually closer to WE.
//   - 15.0 with soft saturation — peaks land around 0.65–0.85 thanks to
//     the sqrt-first + x/(1+0.4x) curve (analytical asymptote 2.5), so the
//     old 15.0+hard-clamp saturation problem doesn't return.  Driver:
//     user explicitly wanted maximum visible amplitude on visualizers.
constexpr float SPECTRUM_GAIN = 15.0f;

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
            float    logFreq = logMin + (logMax - logMin) * (float)b / (float)NUM_BANDS_64;
            float    freq    = std::pow(10.0f, logFreq);
            uint32_t bin     = (uint32_t)(freq / freqPerBin + 0.5f);
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
    // MPSC ring buffer for PCM transfer from the audio threads.  Two miniaudio
    // data callbacks may call FeedPcm concurrently — AudioCapture's PipeWire
    // monitor (AudioCapture.cpp captureCallback) and SoundManager's playback
    // spectrum tap (SoundManager.cpp SetSpectrumCallback closure).  The
    // PROPERTY_SYSTEM_AUDIO_CAPTURE toggle briefly leaves both active during
    // the transition window, racing on ring[wp % RING_SIZE].  feedMutex
    // serializes the producers so their ring stores never interleave; the
    // consumer (Process) reads lock-free via writePos acquire and is
    // unaffected by this mutex.
    std::array<float, RING_SIZE> ring {};
    std::atomic<uint32_t>        writePos { 0 };
    uint32_t                     readPos { 0 };
    std::mutex                   feedMutex;

    // FFT state
    kiss_fftr_cfg                      fftCfg { nullptr };
    std::array<float, FFT_SIZE>        windowedL {};
    std::array<float, FFT_SIZE>        windowedR {};
    std::array<kiss_fft_cpx, NUM_BINS> freqL {};
    std::array<kiss_fft_cpx, NUM_BINS> freqR {};
    std::array<float, NUM_BINS>        magL {};
    std::array<float, NUM_BINS>        magR {};

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

    // Test-only: cumulative count of FFT windows computed since construction.
    // Incremented inside the Process loop once per FFT (i.e. once per
    // FFT_SIZE-stereo-frame overlap stride).  Read by WindowsProcessedForTest().
    uint64_t windowsProcessed { 0 };

    // One-shot diagnostic breadcrumbs.  The overlap rewrite shifted Process()
    // from a single FFT-per-call to a while-loop that runs N FFTs in one
    // invocation (50% overlap stride); the bass-band integrator inside
    // DoOneFFTWindow gains energy as bass-frequency content lands in the
    // first few log-spaced bands.  Each breadcrumb is fired ONCE per analyzer
    // instance so the journal proves the new path is engaged at runtime
    // without flooding (Process runs at 60Hz; per-frame LOG_INFO would
    // wallpaper the log).  Both flags survive across analyzer lifetime per
    // [[feedback_keep_debug_logging]] — future investigations of audio
    // wallpapers benefit from a single "saw N FFTs this call" / "bass band
    // active" line in the journal at scene start.
    bool loggedOverlapMultiWindow { false };
    bool loggedBassBandActive { false };

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

    // Single FFT window starting at rp_start (ring index in float units;
    // caller validated wp - rp_start >= FFT_SIZE * 2).  Reads
    // ring[(rp_start + i*2) % RING_SIZE] for i in [0, FFT_SIZE), applies
    // Hanning, runs kiss_fftr, accumulates magnitudes into smoothL64/R64 via
    // the existing exponential filter, and refreshes rawL/R{64,32,16} +
    // padL/R{64,32,16}.  Called in a loop by Process() — each successive call
    // shares half its window with the previous one (50% Hanning-COLA overlap).
    void DoOneFFTWindow(uint32_t rp_start);
};

void AudioAnalyzer::Impl::DoOneFFTWindow(uint32_t rp_start) {
    // Deinterleave + apply Hanning window
    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        uint32_t idx = (rp_start + i * 2) % RING_SIZE;
        windowedL[i] = ring[idx] * g_hanning.w[i];
        windowedR[i] = ring[(idx + 1) % RING_SIZE] * g_hanning.w[i];
    }
    windowsProcessed++;

    // FFT
    kiss_fftr(fftCfg, windowedL.data(), freqL.data());
    kiss_fftr(fftCfg, windowedR.data(), freqR.data());

    // Magnitude
    float norm = 2.0f / (float)FFT_SIZE;
    for (uint32_t k = 0; k < NUM_BINS; k++) {
        float reL = freqL[k].r, imL = freqL[k].i;
        float reR = freqR[k].r, imR = freqR[k].i;
        magL[k] = std::sqrt(reL * reL + imL * imL) * norm;
        magR[k] = std::sqrt(reR * reR + imR * imR) * norm;
    }
    // DC (k=0) and Nyquist (k=NUM_BINS-1 == FFT_SIZE/2) are self-conjugate
    // in a real-input FFT — they have no negative-frequency mirror to fold
    // back, so the *2 in `norm` over-counts them.  Halve to correct.
    magL[0] *= 0.5f;
    magR[0] *= 0.5f;
    magL[NUM_BINS - 1] *= 0.5f;
    magR[NUM_BINS - 1] *= 0.5f;

    // Map bins to 64 log-spaced bands
    for (uint32_t b = 0; b < NUM_BANDS_64; b++) {
        uint32_t lo = bandMap.edges[b];
        uint32_t hi = bandMap.edges[b + 1];
        if (hi <= lo) hi = lo + 1;
        if (hi > NUM_BINS) hi = NUM_BINS;
        float sumL = 0, sumR = 0;
        for (uint32_t k = lo; k < hi; k++) {
            sumL += magL[k];
            sumR += magR[k];
        }
        float n = (float)(hi - lo);
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
        if (newL >= smoothL64[b])
            smoothL64[b] = newL * SMOOTHING_ATTACK + smoothL64[b] * (1.0f - SMOOTHING_ATTACK);
        else
            smoothL64[b] = newL * (1.0f - SMOOTHING_DECAY) + smoothL64[b] * SMOOTHING_DECAY;

        if (newR >= smoothR64[b])
            smoothR64[b] = newR * SMOOTHING_ATTACK + smoothR64[b] * (1.0f - SMOOTHING_ATTACK);
        else
            smoothR64[b] = newR * (1.0f - SMOOTHING_DECAY) + smoothR64[b] * SMOOTHING_DECAY;

        rawL64[b] = smoothL64[b];
        rawR64[b] = smoothR64[b];
    }

    // One-shot bass-band breadcrumb: bands 0..3 of the 64-band log-spaced
    // map cover ~20-100Hz (kick, sub-bass, low bass).  Under the 50%-overlap
    // path each FFT integrates phase-coherently into smoothL64, so
    // bass-frequency content lands here cleanly rather than as the
    // noisy single-window snapshot that the legacy code produced.  Log
    // ONCE when any low band first crosses an audible threshold — useful
    // when investigating why a future audio-reactive wallpaper does not
    // pulse on bass.
    if (! loggedBassBandActive) {
        float lowBandMax = 0.0f;
        for (uint32_t b = 0; b < 4; b++) {
            lowBandMax = std::max(lowBandMax, smoothL64[b]);
            lowBandMax = std::max(lowBandMax, smoothR64[b]);
        }
        if (lowBandMax > 0.1f) {
            loggedBassBandActive = true;
            LOG_INFO("AudioAnalyzer: bass band (20-100Hz, smoothL/R64[0..3]) "
                     "first active (peak=%.3f)",
                     (double)lowBandMax);
        }
    }

    // 32 bands = pairwise average of 64
    for (uint32_t b = 0; b < NUM_BANDS_32; b++) {
        rawL32[b] = (rawL64[b * 2] + rawL64[b * 2 + 1]) * 0.5f;
        rawR32[b] = (rawR64[b * 2] + rawR64[b * 2 + 1]) * 0.5f;
    }

    // 16 bands = pairwise average of 32
    for (uint32_t b = 0; b < NUM_BANDS_16; b++) {
        rawL16[b] = (rawL32[b * 2] + rawL32[b * 2 + 1]) * 0.5f;
        rawR16[b] = (rawR32[b * 2] + rawR32[b * 2 + 1]) * 0.5f;
    }

    // Generate std140-padded output
    PadSpectrum(rawL16.data(), padL16.data(), NUM_BANDS_16);
    PadSpectrum(rawR16.data(), padR16.data(), NUM_BANDS_16);
    PadSpectrum(rawL32.data(), padL32.data(), NUM_BANDS_32);
    PadSpectrum(rawR32.data(), padR32.data(), NUM_BANDS_32);
    PadSpectrum(rawL64.data(), padL64.data(), NUM_BANDS_64);
    PadSpectrum(rawR64.data(), padR64.data(), NUM_BANDS_64);

    hasData = true;
}

AudioAnalyzer::AudioAnalyzer(): m_impl(std::make_unique<Impl>()) {}
AudioAnalyzer::~AudioAnalyzer() = default;

void AudioAnalyzer::FeedPcm(const float* interleavedStereo, uint32_t frameCount,
                            uint32_t channels) {
    // Write interleaved stereo samples into ring buffer.  MPSC: AudioCapture's
    // PipeWire monitor and SoundManager's playback spectrum tap may both call
    // here during the audio-capture toggle window; feedMutex serializes their
    // writes so ring[wp % RING_SIZE] stores never interleave.  The consumer
    // (Process) is lock-free via writePos acquire and never blocks on this
    // mutex.  If mono, duplicate to L+R; if >2 channels, take first 2.
    if (frameCount == 0) return; // miniaudio's empty STARTED callback hits here
    std::lock_guard<std::mutex> lock(m_impl->feedMutex);
    uint32_t                    wp = m_impl->writePos.load(std::memory_order_relaxed);
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
    WEK_PROFILE_SCOPE("AudioAnalyzer::Process");
    auto& d = *m_impl;

    // 50% overlap: run AS MANY FFT windows as fit in the unread span, advancing
    // readPos by FFT_SIZE per window (half of the 2*FFT_SIZE-float window — the
    // standard Hanning COLA reconstruction stride).  Multi-window-per-tick
    // integrates into the existing exponential smoothing so the smoothed
    // spectrum becomes phase-coherent at no extra accumulator state.  Previously
    // Process jumped readPos = wp after one FFT, discarding both the FFT's input
    // and the ~288 stereo frames written per render tick that fell outside the
    // 1024-float window — bass content (40-150Hz, period 6.7-25ms straddling the
    // 10.7ms FFT window) was under-resolved as each FFT caught a different
    // phase of the bass.  Made safe by RING_SIZE = 32768 (T6 widening): the
    // consumer's working span can grow to ~330ms before the producer laps.
    uint32_t wp = d.writePos.load(std::memory_order_acquire);
    uint32_t rp = d.readPos;

    uint32_t available = wp - rp; // wraps correctly for unsigned
    if (available < FFT_SIZE * 2) {
        // Not enough data yet for even one FFT window
        return;
    }

    uint32_t windowsThisCall = 0;
    while (wp - rp >= FFT_SIZE * 2) {
        d.DoOneFFTWindow(rp);
        rp += FFT_SIZE; // 50% overlap stride (half a window)
        windowsThisCall++;
    }
    d.readPos = rp; // next-start, NOT wp — leaves residual (< FFT_SIZE*2 floats)
                    // for the next call to combine with new samples

    // One-shot breadcrumb: confirm the 50%-overlap branch ran more than the
    // legacy single-window count (pre-rewrite Process always executed
    // exactly one FFT per call).  Fires the first time we observe a
    // multi-window tick so the journal records "overlap engaged" without
    // looping at 60Hz.
    if (windowsThisCall >= 2 && ! d.loggedOverlapMultiWindow) {
        d.loggedOverlapMultiWindow = true;
        LOG_INFO("AudioAnalyzer::Process: 50%% overlap engaged "
                 "(%u FFT windows in one call; legacy path was 1)",
                 windowsThisCall);
    }
}

std::span<const float> AudioAnalyzer::GetSpectrum16Left() const { return m_impl->padL16; }
std::span<const float> AudioAnalyzer::GetSpectrum16Right() const { return m_impl->padR16; }
std::span<const float> AudioAnalyzer::GetSpectrum32Left() const { return m_impl->padL32; }
std::span<const float> AudioAnalyzer::GetSpectrum32Right() const { return m_impl->padR32; }
std::span<const float> AudioAnalyzer::GetSpectrum64Left() const { return m_impl->padL64; }
std::span<const float> AudioAnalyzer::GetSpectrum64Right() const { return m_impl->padR64; }

std::span<const float> AudioAnalyzer::GetRawSpectrum(int resolution, int channel) const {
    auto& d = *m_impl;
    // channel: 0=left, 1=right
    switch (resolution) {
    case 16:
        return channel == 0 ? std::span<const float>(d.rawL16) : std::span<const float>(d.rawR16);
    case 32:
        return channel == 0 ? std::span<const float>(d.rawL32) : std::span<const float>(d.rawR32);
    case 64:
        return channel == 0 ? std::span<const float>(d.rawL64) : std::span<const float>(d.rawR64);
    default: return {};
    }
}

bool AudioAnalyzer::HasData() const { return m_impl->hasData; }

uint64_t AudioAnalyzer::WindowsProcessedForTest() const { return m_impl->windowsProcessed; }
