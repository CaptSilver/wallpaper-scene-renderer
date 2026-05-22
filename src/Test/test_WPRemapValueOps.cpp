#include <doctest.h>
#include "Particle/RemapValueOps.hpp"

#include "WPParticleParser.hpp"
#include "Particle/Particle.h"
#include "Particle/ParticleEmitter.h"
#include "Particle/BlendWindow.h"
#include "wpscene/WPParticleObject.h"

#include <nlohmann/json.hpp>
#include <Eigen/Core>
#include <cmath>
#include <vector>
#include <array>

using namespace wallpaper;
using nlohmann::json;

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

namespace
{
struct RVFixture {
    std::vector<Particle>               particles;
    std::array<ParticleControlpoint, 8> cps;
    double                              time { 0.0 };
    ParticleInfo                        info() {
        return ParticleInfo {
                                   .particles = std::span<Particle>(particles),
                                   .controlpoints = std::span<ParticleControlpoint>(cps.data(), cps.size()),
                                   .time      = time,
                                   .time_pass = 0.016,
        };
    }
    Particle& spawn() {
        Particle p;
        p.lifetime        = 0.5f;
        p.init.lifetime   = 1.0f; // LifetimePos = 0.5
        p.alpha           = 0.4f;
        p.size            = 20.0f;
        p.color           = Eigen::Vector3f(0.2f, 0.5f, 0.8f);
        p.velocity        = Eigen::Vector3f(3.0f, -4.0f, 12.0f);
        p.rotation        = Eigen::Vector3f(0.1f, 0.2f, 0.3f);
        p.angularVelocity = Eigen::Vector3f(0.5f, 0.0f, -0.5f);
        p.position        = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
        p.random_seed     = 0xdeadbeef;
        particles.push_back(p);
        return particles.back();
    }
};
wpscene::ParticleInstanceoverride no_override() {
    wpscene::ParticleInstanceoverride o;
    o.enabled = false;
    return o;
}
} // namespace

TEST_SUITE("remapvalue operator end-to-end equivalence") {
    // Alpha output from particlelifetime, linear, set — exercises the scalar
    // output path + the SetRemap operation.
    TEST_CASE("lifetime -> alpha, set, linear") {
        RVFixture f;
        f.spawn();
        json wpj = { { "name", "remapvalue" },  { "input", "particlelifetime" },
                     { "output", "alpha" },     { "operation", "set" },
                     { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        auto op  = WPParticleParser::genParticleOperatorOp(wpj, no_override());
        op(f.info());
        // raw=LifetimePos=0.5; t=0.5; identity; mapped=0.5; blend=1; set => 0.5
        CHECK(f.particles[0].alpha == doctest::Approx(0.5f));
    }
    // velocity magnitude -> size, multiply default — exercises reduce(magnitude)
    // + multiply default.
    TEST_CASE("velocity(magnitude) -> size, multiply default") {
        RVFixture f;
        f.spawn();
        json wpj = { { "name", "remapvalue" },          { "input", "particlevelocity" },
                     { "inputcomponent", "magnitude" }, { "output", "size" },
                     { "inputrangemin", 0.0 },          { "inputrangemax", 13.0 },
                     { "outputrangemin", 0.0 },         { "outputrangemax", 2.0 } };
        auto op  = WPParticleParser::genParticleOperatorOp(wpj, no_override());
        // |v|=13; t=1; mapped=2; multiply: size*(1+(2-1)*1)=size*2 = 40
        op(f.info());
        CHECK(f.particles[0].size == doctest::Approx(40.0f));
    }
    // color vec3 broadcast vs single-axis outputcomponent.
    TEST_CASE("particlesystemtime -> color, single axis x") {
        RVFixture f;
        f.spawn();
        f.time   = 1.0;
        json wpj = { { "name", "remapvalue" },      { "input", "particlesystemtime" },
                     { "output", "particlecolor" }, { "operation", "set" },
                     { "outputcomponent", "x" },    { "inputrangemin", 0.0 },
                     { "inputrangemax", 1.0 },      { "outputrangemin", 0.0 },
                     { "outputrangemax", 1.0 } };
        auto op  = WPParticleParser::genParticleOperatorOp(wpj, no_override());
        op(f.info());
        CHECK(f.particles[0].color.x() == doctest::Approx(1.0f)); // x set to mapped=1
        CHECK(f.particles[0].color.y() == doctest::Approx(0.5f)); // y untouched
        CHECK(f.particles[0].color.z() == doctest::Approx(0.8f)); // z untouched
    }
    // Alias parity: `color` input == `particlecolor`; `velocity` output ==
    // `particlevelocity`; `setvelocity` prefix-sugar == output velocity + set.
    TEST_CASE("aliases resolve identically (color, velocity, setvelocity)") {
        // `setvelocity` (prefix sugar) -> output particlevelocity, operation set
        RVFixture f;
        f.spawn();
        json wpj = { { "name", "remapvalue" },    { "input", "particlesystemtime" },
                     { "output", "setvelocity" }, { "inputrangemin", 0.0 },
                     { "inputrangemax", 1.0 },    { "outputrangemin", 0.0 },
                     { "outputrangemax", 0.0 } };
        f.time   = 1.0;
        auto op  = WPParticleParser::genParticleOperatorOp(wpj, no_override());
        op(f.info());
        // mapped=0; set => velocity broadcast to 0 on all axes
        CHECK(f.particles[0].velocity.norm() == doctest::Approx(0.0f));
    }
    // Unknown input -> raw stays 0 -> mapped=outMin; unknown output -> no-op.
    TEST_CASE("unknown input is a defined no-effect path") {
        RVFixture f;
        f.spawn();
        float before = f.particles[0].alpha;
        json  wpj    = { { "name", "remapvalue" },
                         { "input", "notarealinput" },
                         { "output", "position" } }; // output unhandled
        auto  op     = WPParticleParser::genParticleOperatorOp(wpj, no_override());
        op(f.info());
        CHECK(f.particles[0].alpha == doctest::Approx(before)); // untouched
    }
    // Control-point input + output round-trips through the cps span.
    TEST_CASE("controlpoint input/output uses the cps span") {
        RVFixture f;
        f.spawn();
        f.cps[0].resolved = Eigen::Vector3d(1.0, 0.0, 0.0);
        json wpj          = { { "name", "remapvalue" },    { "input", "distancetocontrolpoint" },
                              { "inputcontrolpoint0", 0 }, { "output", "alpha" },
                              { "operation", "set" },      { "inputrangemin", 0.0 },
                              { "inputrangemax", 9.0 },    { "outputrangemin", 0.0 },
                              { "outputrangemax", 1.0 } };
        auto op           = WPParticleParser::genParticleOperatorOp(wpj, no_override());
        op(f.info());
        // dist(|(10,0,0)-(1,0,0)|)=9; t=1; mapped=1; set => alpha 1
        CHECK(f.particles[0].alpha == doctest::Approx(1.0f));
    }
}
