#include <doctest.h>
#include "Particle/RemapValueOps.hpp"

using namespace wallpaper;

TEST_SUITE("RemapValueOps resolvers") {
    TEST_CASE("input strings map to their enum, with aliases") {
        CHECK(parseRemapInput("particlesystemtime")    == RemapInput::ParticleSystemTime);
        CHECK(parseRemapInput("particlelifetime")      == RemapInput::ParticleLifetime);
        CHECK(parseRemapInput("particlevelocity")      == RemapInput::ParticleVelocity);
        CHECK(parseRemapInput("particleposition")      == RemapInput::ParticlePosition);
        CHECK(parseRemapInput("particlerotation")      == RemapInput::ParticleRotation);
        CHECK(parseRemapInput("particleangularvelocity") == RemapInput::ParticleAngularVelocity);
        CHECK(parseRemapInput("particlecolor")         == RemapInput::ParticleColor);
        CHECK(parseRemapInput("color")                 == RemapInput::ParticleColor);   // alias
        CHECK(parseRemapInput("particlesize")          == RemapInput::ParticleSize);
        CHECK(parseRemapInput("size")                  == RemapInput::ParticleSize);    // alias
        CHECK(parseRemapInput("particlealpha")         == RemapInput::ParticleAlpha);
        CHECK(parseRemapInput("opacity")               == RemapInput::ParticleAlpha);   // alias
        CHECK(parseRemapInput("speed")                 == RemapInput::Speed);
        CHECK(parseRemapInput("angularspeed")          == RemapInput::AngularSpeed);
        CHECK(parseRemapInput("maxlifetime")           == RemapInput::MaxLifetime);
        CHECK(parseRemapInput("runtime")               == RemapInput::Runtime);
        CHECK(parseRemapInput("layertime")             == RemapInput::Runtime);         // alias
        CHECK(parseRemapInput("timeofday")             == RemapInput::TimeOfDay);
        CHECK(parseRemapInput("controlpoint")          == RemapInput::ControlPoint);
        CHECK(parseRemapInput("controlpointposition")  == RemapInput::ControlPoint);    // alias
        CHECK(parseRemapInput("controlpointvelocity")  == RemapInput::ControlPointVelocity);
        CHECK(parseRemapInput("distancetocontrolpoint")   == RemapInput::DistanceToControlPoint);
        CHECK(parseRemapInput("deltatocontrolpoint")      == RemapInput::DeltaToControlPoint);
        CHECK(parseRemapInput("directiontocontrolpoint")  == RemapInput::DirectionToControlPoint);
        CHECK(parseRemapInput("positionbetweentwocontrolpoints") == RemapInput::PositionBetweenTwoControlPoints);
        CHECK(parseRemapInput("layerorigin")           == RemapInput::LayerOrigin);
        CHECK(parseRemapInput("random")                == RemapInput::Random);
        CHECK(parseRemapInput("noise")                 == RemapInput::Noise);
        CHECK(parseRemapInput("")                      == RemapInput::Unknown);
        CHECK(parseRemapInput("notarealinput")         == RemapInput::Unknown);
    }
    TEST_CASE("transform strings map to their enum; unknown -> Identity") {
        CHECK(parseRemapTransform("sine")        == RemapTransform::Sine);
        CHECK(parseRemapTransform("cosine")      == RemapTransform::Cosine);
        CHECK(parseRemapTransform("square")      == RemapTransform::Square);
        CHECK(parseRemapTransform("saw")         == RemapTransform::Saw);
        CHECK(parseRemapTransform("triangle")    == RemapTransform::Triangle);
        CHECK(parseRemapTransform("simplexnoise")== RemapTransform::SimplexNoise);
        CHECK(parseRemapTransform("fbmnoise")    == RemapTransform::FbmNoise);
        CHECK(parseRemapTransform("step")        == RemapTransform::Step);
        CHECK(parseRemapTransform("smoothstep")  == RemapTransform::Smoothstep);
        CHECK(parseRemapTransform("none")        == RemapTransform::Identity);
        CHECK(parseRemapTransform("linear")      == RemapTransform::Identity);
        CHECK(parseRemapTransform("whatever")    == RemapTransform::Identity);
    }
    TEST_CASE("operation strings map to their enum; unknown -> Multiply") {
        CHECK(parseRemapOperation("set")      == RemapOperation::SetRemap);
        CHECK(parseRemapOperation("remap")    == RemapOperation::SetRemap);  // alias
        CHECK(parseRemapOperation("add")      == RemapOperation::Add);
        CHECK(parseRemapOperation("subtract") == RemapOperation::Subtract);
        CHECK(parseRemapOperation("multiply") == RemapOperation::Multiply);
        CHECK(parseRemapOperation("")         == RemapOperation::Multiply);  // default
    }
    TEST_CASE("output strings map to their enum; unknown -> Unhandled") {
        CHECK(parseRemapOutput("opacity")        == RemapOutput::Alpha);
        CHECK(parseRemapOutput("particlealpha")  == RemapOutput::Alpha);
        CHECK(parseRemapOutput("alpha")          == RemapOutput::Alpha);
        CHECK(parseRemapOutput("size")           == RemapOutput::Size);
        CHECK(parseRemapOutput("particlesize")   == RemapOutput::Size);
        CHECK(parseRemapOutput("particlevelocity")        == RemapOutput::Velocity);
        CHECK(parseRemapOutput("particleangularvelocity") == RemapOutput::AngularVelocity);
        CHECK(parseRemapOutput("particlerotation")        == RemapOutput::Rotation);
        CHECK(parseRemapOutput("particlecolor")  == RemapOutput::Color);
        CHECK(parseRemapOutput("controlpoint")   == RemapOutput::ControlPoint);
        CHECK(parseRemapOutput("position")       == RemapOutput::Unhandled);
        CHECK(parseRemapOutput("")               == RemapOutput::Unhandled);
    }
    TEST_CASE("component strings map to their enum; all/magnitude/unknown -> Norm") {
        CHECK(parseRemapComponent("x")        == RemapComponent::X);
        CHECK(parseRemapComponent("y")        == RemapComponent::Y);
        CHECK(parseRemapComponent("z")        == RemapComponent::Z);
        CHECK(parseRemapComponent("w")        == RemapComponent::W);
        CHECK(parseRemapComponent("x+y")      == RemapComponent::XPlusY);
        CHECK(parseRemapComponent("sum")      == RemapComponent::Sum);
        CHECK(parseRemapComponent("average")  == RemapComponent::Average);
        CHECK(parseRemapComponent("max")      == RemapComponent::Max);
        CHECK(parseRemapComponent("min")      == RemapComponent::Min);
        CHECK(parseRemapComponent("all")       == RemapComponent::Norm);
        CHECK(parseRemapComponent("magnitude") == RemapComponent::Norm);
        CHECK(parseRemapComponent("")          == RemapComponent::Norm);
    }
}
