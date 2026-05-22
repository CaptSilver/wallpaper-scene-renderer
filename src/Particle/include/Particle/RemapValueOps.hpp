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

} // namespace wallpaper
