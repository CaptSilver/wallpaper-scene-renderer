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

// Map a frame of FFT spectrum + previous smoothed bass amplitude into an
// emit-rate multiplier for an audio-reactive particle subsystem.
//
// Pure function (no statics, no globals).  Used by SceneWallpaper to push a
// per-frame multiplier into ParticleSubSystem::SetAudioRateMultiplier when
// the subsystem's `audioprocessingmode` is non-zero.
//
// `mode == 0`              -> {1.0, 0.0} (caller stores 0 so re-enabling is clean).
// `spectrum.empty()`       -> {1.0, prevSmoothed} (no FFT data this frame, hold).
// otherwise:
//   bass = mean of spectrum[0..3] clamped to [0, 1]
//   tau  = 0.05s on attack (bass > prev), 0.30s on decay
//   alpha = 1 - exp(-dt / max(tau, eps))
//   newSmoothed = prev + alpha * (bass - prev)
//   multiplier  = 0.4 + 1.6 * newSmoothed     (silence 0.4x, full 2.0x, nominal ~1.0x)
//
// Smoothing makes this stable across small FFT jitter; fast attack + slow
// decay keeps the visual feel of a beat (snap up, fade down) rather than a
// symmetric envelope follower.
inline RateMultiplierResult computeRateMultiplier(std::span<const float> spectrum16,
                                                  double                 prevSmoothed,
                                                  double                 dtSec,
                                                  uint32_t               mode) {
    if (mode == 0) return { 1.0, 0.0 };
    if (spectrum16.empty()) return { 1.0, prevSmoothed };

    double bass = 0.0;
    int    n    = std::min<int>(4, static_cast<int>(spectrum16.size()));
    for (int i = 0; i < n; i++) bass += spectrum16[i];
    bass /= n;
    bass = std::clamp(bass, 0.0, 1.0);

    constexpr double kAttackTau = 0.05;
    constexpr double kDecayTau  = 0.30;
    constexpr double kEps       = 1e-6;
    double           tau        = (bass > prevSmoothed) ? kAttackTau : kDecayTau;
    double           alpha      = 1.0 - std::exp(-dtSec / std::max(tau, kEps));
    double           smoothed   = prevSmoothed + alpha * (bass - prevSmoothed);

    constexpr double kFloor = 0.4;
    constexpr double kSpan  = 1.6; // 0.4 + 1.6 = 2.0 ceiling
    double           mult   = kFloor + kSpan * smoothed;
    return { mult, smoothed };
}

} // namespace wallpaper::audio_reactive
