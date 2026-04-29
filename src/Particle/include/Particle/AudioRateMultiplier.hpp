#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace wallpaper::audio_reactive
{

struct RateMultiplierResult {
    double multiplier;   // emit-rate scalar (1.0 = unchanged)
    double newSmoothed;  // updated smoothing state, store on the subsystem
};

// Per-emitter audio response parameters.  `mode` is the only required field;
// when authored alone the legacy 0.4..2.0 curve over the bass band [0..4) is
// used (visually validated against multiple wallpapers).  When any of the
// optional WE-shape keys are authored on the scene-side emitter, the response
// switches to the WE-spec smoothstep(bounds, mean) ^ exp * amount curve.
struct RateMultiplierParams {
    // `audioprocessingoptions` enum on the emitter, shared with the shader-side
    // AUDIOPROCESSING combo.  0=off, 1=Left, 2=Right, 3=Stereo (mean of L+R).
    uint32_t mode { 0 };

    // True when at least one of the optional WE-shape keys
    // (audioprocessingfrequencystart/end, audioprocessingbounds,
    // audioprocessingexponent, audioprocessing) was authored on the emitter.
    // Selects the WE-shape curve over the legacy curve.
    bool weShapeAuthored { false };

    // Inclusive start / exclusive end into the 16-band spectrum.  Defaults
    // cover the bass band [0..4) used by the legacy curve.
    int freqStart { 0 };
    int freqEnd { 4 };

    // smoothstep input range for the WE-shape curve.  Default [0, 1] is the
    // identity smoothstep (saturates only at the edges).
    float boundsLow { 0.0f };
    float boundsHigh { 1.0f };

    // pow exponent for the WE-shape curve.  1.0 is linear; >1 sharpens; <1
    // softens.  WE editor caps this at 4.
    float exponent { 1.0f };

    // Post-multiplier for the WE-shape curve.  WE editor caps this at 2 so the
    // response can at most double the base rate.
    float amount { 1.0f };
};

namespace detail
{

inline double smoothstep(double edge0, double edge1, double x) {
    if (edge1 <= edge0) return x >= edge1 ? 1.0 : 0.0;
    double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

inline double bandMean(std::span<const float> spec, int start, int endExclusive) {
    if (spec.empty()) return 0.0;
    int n  = static_cast<int>(spec.size());
    int lo = std::clamp(start, 0, n);
    int hi = std::clamp(endExclusive, lo, n);
    if (hi <= lo) return 0.0;
    double sum = 0.0;
    for (int i = lo; i < hi; i++) sum += spec[i];
    return sum / static_cast<double>(hi - lo);
}

inline double channelMean(std::span<const float> left, std::span<const float> right,
                          int start, int endExclusive, uint32_t mode) {
    switch (mode) {
    case 1: return bandMean(left, start, endExclusive);
    case 2: return bandMean(right, start, endExclusive);
    case 3: {
        // Stereo combined — element-wise mean simplifies to scalar mean of
        // each channel's band.  When a channel is empty it contributes 0,
        // matching "treat missing channel as silence".
        double l = bandMean(left, start, endExclusive);
        double r = bandMean(right, start, endExclusive);
        return 0.5 * (l + r);
    }
    default: return 0.0;
    }
}

} // namespace detail

// Map a frame of FFT spectrum (left + right channels) + previous smoothed
// state into an emit-rate multiplier for an audio-reactive particle subsystem.
//
// Pure function (no statics, no globals).  Used by SceneWallpaper to push a
// per-frame multiplier into ParticleSubSystem::SetAudioRateMultiplier when
// the subsystem is audio-reactive.
//
// `mode == 0`              -> {1.0, 0.0} (caller stores 0 so re-enabling is clean).
// no FFT data this frame   -> {1.0, prevSmoothed} (hold).
// otherwise:
//   bandMean = mean(spectrum[freqStart..freqEnd)) for the requested channel(s)
//   smoothed = exp-filtered (tau=0.05s on attack, 0.30s on decay)
//   curve    = legacy 0.4 + 1.6*smoothed   when !weShapeAuthored
//            = smoothstep(bounds, smoothed)^exp * amount  when weShapeAuthored
//
// Smoothing makes the response stable across small FFT jitter; fast attack +
// slow decay keeps the visual feel of a beat (snap up, fade down).
inline RateMultiplierResult computeRateMultiplier(std::span<const float>     spectrumLeft,
                                                  std::span<const float>     spectrumRight,
                                                  double                     prevSmoothed,
                                                  double                     dtSec,
                                                  const RateMultiplierParams& params) {
    if (params.mode == 0) return { 1.0, 0.0 };

    bool noLeft  = spectrumLeft.empty();
    bool noRight = spectrumRight.empty();
    bool noData  = (params.mode == 1 && noLeft) ||
                  (params.mode == 2 && noRight) ||
                  (params.mode == 3 && noLeft && noRight);
    if (noData) return { 1.0, prevSmoothed };

    double mean = detail::channelMean(
        spectrumLeft, spectrumRight, params.freqStart, params.freqEnd, params.mode);
    mean = std::clamp(mean, 0.0, 1.0);

    constexpr double kAttackTau = 0.05;
    constexpr double kDecayTau  = 0.30;
    constexpr double kEps       = 1e-6;
    double           tau        = (mean > prevSmoothed) ? kAttackTau : kDecayTau;
    double           alpha      = 1.0 - std::exp(-dtSec / std::max(tau, kEps));
    double           smoothed   = prevSmoothed + alpha * (mean - prevSmoothed);

    double mult;
    if (! params.weShapeAuthored) {
        constexpr double kFloor = 0.4;
        constexpr double kSpan  = 1.6; // 0.4 + 1.6 = 2.0 ceiling
        mult                    = kFloor + kSpan * smoothed;
    } else {
        double s = detail::smoothstep(params.boundsLow, params.boundsHigh, smoothed);
        s        = std::clamp(s, 0.0, 1.0);
        if (params.exponent != 1.0f)
            s = std::pow(std::max(s, 0.0), static_cast<double>(params.exponent));
        mult = s * static_cast<double>(params.amount);
    }
    return { mult, smoothed };
}

} // namespace wallpaper::audio_reactive
