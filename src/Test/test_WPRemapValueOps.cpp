#include <doctest.h>
#include "Particle/RemapValueOps.hpp"

#include <Eigen/Core>
#include <cmath>

using namespace wallpaper;

using Vec3 = Eigen::Vector3d;

namespace {
// Oracle: faithful copy of the CURRENT reduce() string switch.
double reduceOracle(const Vec3& v, std::string_view comp) {
    if (comp == "x") return v.x();
    if (comp == "y") return v.y();
    if (comp == "z") return v.z();
    if (comp == "w") return 0.0;
    if (comp == "x+y") return v.x() + v.y();
    if (comp == "sum") return v.x() + v.y() + v.z();
    if (comp == "average") return (v.x() + v.y() + v.z()) / 3.0;
    if (comp == "max") return std::max({ v.x(), v.y(), v.z() });
    if (comp == "min") return std::min({ v.x(), v.y(), v.z() });
    return v.norm();
}
// Oracle: faithful copy of the CURRENT transform string switch.
double txOracle(std::string_view fn, double xs, int octaves) {
    if (fn == "sine")   return (std::sin(xs * 2.0 * M_PI) + 1.0) * 0.5;
    if (fn == "cosine") return (std::cos(xs * 2.0 * M_PI) + 1.0) * 0.5;
    if (fn == "square") return std::sin(xs * 2.0 * M_PI) >= 0.0 ? 1.0 : 0.0;
    if (fn == "saw")    return xs - std::floor(xs);
    if (fn == "triangle") { const double f = xs - std::floor(xs); return std::abs(2.0 * f - 1.0); }
    if (fn == "simplexnoise") {
        const double n = wallpaper::algorism::PerlinNoise(xs, 0.0, 0.0);
        return std::clamp((n + 1.0) * 0.5, 0.0, 1.0);
    }
    if (fn == "fbmnoise") {
        double sum = 0.0, amp = 1.0, freq = 1.0, maxAmp = 0.0;
        for (int oct = 0; oct < octaves; oct++) {
            sum += amp * wallpaper::algorism::PerlinNoise(xs * freq, 0.0, 0.0);
            maxAmp += amp; amp *= 0.5; freq *= 2.0;
        }
        if (maxAmp < 1e-9) maxAmp = 1.0;
        return std::clamp((sum / maxAmp + 1.0) * 0.5, 0.0, 1.0);
    }
    if (fn == "step") return std::floor(xs);
    if (fn == "smoothstep") { double c = std::clamp(xs, 0.0, 1.0); return c * c * (3.0 - 2.0 * c); }
    return xs; // none / linear / unknown
}
// Oracle: faithful copy of the CURRENT apply_scalar operation switch.
double opOracle(std::string_view op, double current, double op_val, double blend) {
    if (op == "set" || op == "remap") return current * (1.0 - blend) + op_val * blend;
    if (op == "add")      return current + op_val * blend;
    if (op == "subtract") return current - op_val * blend;
    return current * (1.0 + (op_val - 1.0) * blend); // multiply
}
} // namespace

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

TEST_SUITE("RemapValueOps kernels match the string oracle") {
    TEST_CASE("reduceComponent equals reduce() for every component") {
        const Vec3 vs[] = { {3, -4, 12}, {0, 0, 0}, {-1.5, 2.5, -7.0}, {1, 1, 1} };
        const char* comps[] = { "x","y","z","w","x+y","sum","average","max","min","magnitude","all","junk" };
        for (auto& v : vs)
            for (auto c : comps)
                CHECK(reduceComponent(v, parseRemapComponent(c)) == doctest::Approx(reduceOracle(v, c)));
    }
    TEST_CASE("applyRemapTransform equals the transform switch for every function") {
        const double xss[] = { 0.0, 0.25, 0.5, 1.0, 2.0, -0.3 };
        const int octs[] = { 1, 4 };
        const char* fns[] = { "sine","cosine","square","saw","triangle","simplexnoise",
                              "fbmnoise","step","smoothstep","linear","none","junk" };
        for (auto xs : xss)
            for (auto oct : octs)
                for (auto fn : fns)
                    CHECK(applyRemapTransform(parseRemapTransform(fn), xs, oct)
                          == doctest::Approx(txOracle(fn, xs, oct)));
    }
    TEST_CASE("applyRemapOperation equals apply_scalar for every operation") {
        const double cur[] = { 0.0, 1.0, 0.5, 2.0 };
        const double opv[] = { 0.0, 1.0, 0.25, 3.0 };
        const double bl[]  = { 0.0, 0.5, 1.0 };
        const char* ops[] = { "set","remap","add","subtract","multiply","junk" };
        for (auto c : cur) for (auto ov : opv) for (auto b : bl) for (auto op : ops)
            CHECK(applyRemapOperation(parseRemapOperation(op), c, ov, b)
                  == doctest::Approx(opOracle(op, c, ov, b)));
    }
}
