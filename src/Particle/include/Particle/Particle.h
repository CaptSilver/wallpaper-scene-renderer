#pragma once

#include <Eigen/Core>

namespace wallpaper
{

struct Particle {
    struct InitValue {
        Eigen::Vector3f color { 1.0f, 1.0f, 1.0f };
        float           alpha { 1.0f };
        float           size { 20 };
        float           lifetime { 1.0f };
    };
    Eigen::Vector3f position { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f color { 1.0f, 1.0f, 1.0f };
    float           alpha { 1.0f };
    float           size { 20 };
    float           lifetime { 1.0f };

    Eigen::Vector3f rotation { 0.0f, 0.0f, 0.0f }; // radian  z x y
    Eigen::Vector3f velocity { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f acceleration { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f angularVelocity { 0.0f, 0.0f, 0.0f };
    Eigen::Vector3f angularAcceleration { 0.0f, 0.0f, 0.0f };

    bool      mark_new { true };
    InitValue init {};

    // Per-particle stable random seed.  Set once at spawn (see ParticleEmitter)
    // and consumed by operators that want a deterministic per-particle random
    // value across frames — most notably `remapvalue` with `input: random`.
    // 0 marks "not yet seeded" so emitters can lazily assign on first read.
    uint32_t  random_seed { 0 };

    // Stable random in [0, 1] derived from random_seed via a 32-bit hash.
    // Same seed always yields the same float, so a wallpaper that maps a
    // per-particle random to opacity gets a constant alpha for the particle's
    // whole life rather than flickering.
    float RandomFloat() const noexcept {
        // xorshift32 + 24-bit mantissa.  Small, fast, deterministic.
        uint32_t x = random_seed ? random_seed : 0xa5a5a5a5u;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return (x >> 8) * (1.0f / 16777216.0f); // 2^-24
    }
};
} // namespace wallpaper
