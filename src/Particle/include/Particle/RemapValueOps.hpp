#pragma once

#include <string_view>
#include <algorithm>
#include <cmath>
#include <Eigen/Core>
#include "Utils/Algorism.h"

namespace wallpaper
{

// Decoded once at parse time from the never-changing remapvalue JSON strings,
// so the per-particle inner loop switches on integers instead of running
// std::string == ladders for every particle every frame.

enum class RemapInput {
    ParticleSystemTime, ParticleLifetime, ParticleVelocity, ParticlePosition,
    ParticleRotation, ParticleAngularVelocity, ParticleColor, ParticleSize,
    ParticleAlpha, Speed, AngularSpeed, MaxLifetime, Runtime, TimeOfDay,
    ControlPoint, ControlPointVelocity, DistanceToControlPoint, DeltaToControlPoint,
    DirectionToControlPoint, PositionBetweenTwoControlPoints, LayerOrigin,
    Random, Noise,
    Unknown
};
enum class RemapTransform {
    Sine, Cosine, Square, Saw, Triangle, SimplexNoise, FbmNoise, Step, Smoothstep,
    Identity
};
enum class RemapOperation { SetRemap, Add, Subtract, Multiply };
enum class RemapOutput {
    Alpha, Size, Velocity, AngularVelocity, Rotation, Color, ControlPoint, Unhandled
};
enum class RemapComponent { X, Y, Z, W, XPlusY, Sum, Average, Max, Min, Norm };

inline RemapInput parseRemapInput(std::string_view s) noexcept {
    if (s == "particlesystemtime")      return RemapInput::ParticleSystemTime;
    if (s == "particlelifetime")        return RemapInput::ParticleLifetime;
    if (s == "particlevelocity")        return RemapInput::ParticleVelocity;
    if (s == "particleposition")        return RemapInput::ParticlePosition;
    if (s == "particlerotation")        return RemapInput::ParticleRotation;
    if (s == "particleangularvelocity") return RemapInput::ParticleAngularVelocity;
    if (s == "particlecolor" || s == "color")     return RemapInput::ParticleColor;
    if (s == "particlesize"  || s == "size")      return RemapInput::ParticleSize;
    if (s == "particlealpha" || s == "opacity")   return RemapInput::ParticleAlpha;
    if (s == "speed")                   return RemapInput::Speed;
    if (s == "angularspeed")            return RemapInput::AngularSpeed;
    if (s == "maxlifetime")             return RemapInput::MaxLifetime;
    if (s == "runtime" || s == "layertime")       return RemapInput::Runtime;
    if (s == "timeofday")               return RemapInput::TimeOfDay;
    if (s == "controlpoint" || s == "controlpointposition") return RemapInput::ControlPoint;
    if (s == "controlpointvelocity")    return RemapInput::ControlPointVelocity;
    if (s == "distancetocontrolpoint")  return RemapInput::DistanceToControlPoint;
    if (s == "deltatocontrolpoint")     return RemapInput::DeltaToControlPoint;
    if (s == "directiontocontrolpoint") return RemapInput::DirectionToControlPoint;
    if (s == "positionbetweentwocontrolpoints") return RemapInput::PositionBetweenTwoControlPoints;
    if (s == "layerorigin")             return RemapInput::LayerOrigin;
    if (s == "random")                  return RemapInput::Random;
    if (s == "noise")                   return RemapInput::Noise;
    return RemapInput::Unknown;
}
inline RemapTransform parseRemapTransform(std::string_view s) noexcept {
    if (s == "sine")         return RemapTransform::Sine;
    if (s == "cosine")       return RemapTransform::Cosine;
    if (s == "square")       return RemapTransform::Square;
    if (s == "saw")          return RemapTransform::Saw;
    if (s == "triangle")     return RemapTransform::Triangle;
    if (s == "simplexnoise") return RemapTransform::SimplexNoise;
    if (s == "fbmnoise")     return RemapTransform::FbmNoise;
    if (s == "step")         return RemapTransform::Step;
    if (s == "smoothstep")   return RemapTransform::Smoothstep;
    return RemapTransform::Identity; // none / linear / unknown
}
inline RemapOperation parseRemapOperation(std::string_view s) noexcept {
    if (s == "set" || s == "remap") return RemapOperation::SetRemap;
    if (s == "add")                 return RemapOperation::Add;
    if (s == "subtract")            return RemapOperation::Subtract;
    return RemapOperation::Multiply; // default
}
inline RemapOutput parseRemapOutput(std::string_view s) noexcept {
    if (s == "opacity" || s == "particlealpha" || s == "alpha") return RemapOutput::Alpha;
    if (s == "size" || s == "particlesize")        return RemapOutput::Size;
    if (s == "particlevelocity")                   return RemapOutput::Velocity;
    if (s == "particleangularvelocity")            return RemapOutput::AngularVelocity;
    if (s == "particlerotation")                   return RemapOutput::Rotation;
    if (s == "particlecolor")                      return RemapOutput::Color;
    if (s == "controlpoint")                       return RemapOutput::ControlPoint;
    return RemapOutput::Unhandled; // position etc.
}
inline RemapComponent parseRemapComponent(std::string_view s) noexcept {
    if (s == "x")       return RemapComponent::X;
    if (s == "y")       return RemapComponent::Y;
    if (s == "z")       return RemapComponent::Z;
    if (s == "w")       return RemapComponent::W;
    if (s == "x+y")     return RemapComponent::XPlusY;
    if (s == "sum")     return RemapComponent::Sum;
    if (s == "average") return RemapComponent::Average;
    if (s == "max")     return RemapComponent::Max;
    if (s == "min")     return RemapComponent::Min;
    return RemapComponent::Norm; // all / magnitude / unknown
}

inline double reduceComponent(const Eigen::Vector3d& v, RemapComponent c) noexcept {
    switch (c) {
        case RemapComponent::X:       return v.x();
        case RemapComponent::Y:       return v.y();
        case RemapComponent::Z:       return v.z();
        case RemapComponent::W:       return 0.0;
        case RemapComponent::XPlusY:  return v.x() + v.y();
        case RemapComponent::Sum:     return v.x() + v.y() + v.z();
        case RemapComponent::Average: return (v.x() + v.y() + v.z()) / 3.0;
        case RemapComponent::Max:     return std::max({ v.x(), v.y(), v.z() });
        case RemapComponent::Min:     return std::min({ v.x(), v.y(), v.z() });
        case RemapComponent::Norm:    break;
    }
    return v.norm(); // all / magnitude / unknown
}

inline double applyRemapTransform(RemapTransform fn, double xs, int octaves) noexcept {
    switch (fn) {
        case RemapTransform::Sine:   return (std::sin(xs * 2.0 * M_PI) + 1.0) * 0.5;
        case RemapTransform::Cosine: return (std::cos(xs * 2.0 * M_PI) + 1.0) * 0.5;
        case RemapTransform::Square: return std::sin(xs * 2.0 * M_PI) >= 0.0 ? 1.0 : 0.0;
        case RemapTransform::Saw:    return xs - std::floor(xs);
        case RemapTransform::Triangle: {
            const double f = xs - std::floor(xs);
            return std::abs(2.0 * f - 1.0);
        }
        case RemapTransform::SimplexNoise: {
            const double n = algorism::PerlinNoise(xs, 0.0, 0.0);
            return std::clamp((n + 1.0) * 0.5, 0.0, 1.0);
        }
        case RemapTransform::FbmNoise: {
            double sum = 0.0, amp = 1.0, freq = 1.0, maxAmp = 0.0;
            for (int oct = 0; oct < octaves; oct++) {
                sum += amp * algorism::PerlinNoise(xs * freq, 0.0, 0.0);
                maxAmp += amp;
                amp *= 0.5;
                freq *= 2.0;
            }
            if (maxAmp < 1e-9) maxAmp = 1.0;
            return std::clamp((sum / maxAmp + 1.0) * 0.5, 0.0, 1.0);
        }
        case RemapTransform::Step:       return std::floor(xs);
        case RemapTransform::Smoothstep: {
            double c = std::clamp(xs, 0.0, 1.0);
            return c * c * (3.0 - 2.0 * c);
        }
        case RemapTransform::Identity:   break;
    }
    return xs; // none / linear / unknown
}

inline double applyRemapOperation(RemapOperation op, double current, double op_val,
                                  double blend) noexcept {
    switch (op) {
        case RemapOperation::SetRemap: return current * (1.0 - blend) + op_val * blend;
        case RemapOperation::Add:      return current + op_val * blend;
        case RemapOperation::Subtract: return current - op_val * blend;
        case RemapOperation::Multiply: break;
    }
    return current * (1.0 + (op_val - 1.0) * blend); // multiply (default)
}

} // namespace wallpaper
