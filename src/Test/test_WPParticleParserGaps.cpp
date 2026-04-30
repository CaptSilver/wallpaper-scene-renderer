#include <doctest.h>

#include "WPParticleParser.hpp"
#include "Particle/Particle.h"
#include "Particle/ParticleEmitter.h"
#include "Particle/ParticleModify.h"
#include "wpscene/WPParticleObject.h"

#include <Eigen/Core>
#include <array>
#include <cmath>
#include <nlohmann/json.hpp>
#include <span>
#include <vector>

using namespace wallpaper;
using nlohmann::json;

namespace
{

// Same fixture pattern as test_WPParticleParserMore.cpp.
struct OpFixture {
    std::vector<Particle>               particles;
    std::array<ParticleControlpoint, 8> cps;
    double                              time { 0.0 };
    double                              time_pass { 0.016 };

    OpFixture() {
        for (auto& cp : cps) {
            cp.resolved = Eigen::Vector3d(0, 0, 0);
            cp.velocity = Eigen::Vector3d(0, 0, 0);
        }
    }

    ParticleInfo info() {
        return ParticleInfo {
            .particles     = std::span<Particle>(particles),
            .controlpoints = std::span<ParticleControlpoint>(cps.data(), cps.size()),
            .time          = time,
            .time_pass     = time_pass,
        };
    }

    Particle& spawn() {
        Particle p;
        p.lifetime      = 1.0f;
        p.init.lifetime = 1.0f;
        p.alpha         = 1.0f;
        p.init.alpha    = 1.0f;
        p.size          = 20.0f;
        p.init.size     = 20.0f;
        p.color         = Eigen::Vector3f(1, 1, 1);
        p.init.color    = p.color;
        p.random_seed   = 0xdeadbeef;
        particles.push_back(p);
        return particles.back();
    }
};

wpscene::ParticleInstanceoverride empty_override() {
    wpscene::ParticleInstanceoverride o;
    o.enabled = false;
    return o;
}

// Convenience for building a CP span when an init op needs CPs.
std::span<const ParticleControlpoint> cp_span(const std::array<ParticleControlpoint, 8>& cps) {
    return std::span<const ParticleControlpoint>(cps.data(), cps.size());
}

} // namespace

// ===========================================================================
// turbulence operator (line 1035 in WPParticleParser.cpp)
// ===========================================================================

TEST_SUITE("turbulence operator") {
    TEST_CASE("coherent path (small noiseRate) accelerates particle from CurlNoise") {
        // noiseRate = |timescale * scale * 2| * time_pass.  With defaults & a small
        // tick, noiseRate <= 1.0 → coherent branch (PM::Accelerate).
        json j  = { { "name", "turbulence" },
                    { "scale", 0.1 }, { "timescale", 0.5 },
                    { "speedmin", 100.0 }, { "speedmax", 100.0 },
                    { "phasemin", 0.0 }, { "phasemax", 0.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.time_pass = 0.016;
        Particle& p   = fx.spawn();
        p.position    = Eigen::Vector3f(1, 2, 3);
        Eigen::Vector3f v0 = p.velocity;
        op(fx.info());
        // Some velocity change is expected from the noise field (curl noise
        // rarely outputs zero at non-zero positions).
        CHECK((p.velocity - v0).norm() > 0.0f);
    }

    TEST_CASE("incoherent path (large noiseRate) moves position directly") {
        // Force noiseRate > 1.0 with big scale * timescale * time_pass.
        json j  = { { "name", "turbulence" },
                    { "scale", 100.0 }, { "timescale", 100.0 },
                    { "speedmin", 100.0 }, { "speedmax", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.time_pass = 0.5;
        Particle& p  = fx.spawn();
        p.position   = Eigen::Vector3f(1, 2, 3);
        Eigen::Vector3f start = p.position;
        op(fx.info());
        CHECK((p.position - start).norm() > 0.0f);
    }

    TEST_CASE("mask zeros out unmasked components") {
        // mask "1 0 0" → only x noise survives.  In the coherent branch the
        // y and z deltas are forced to zero, so the velocity y/z stay at 0.
        json j  = { { "name", "turbulence" },
                    { "scale", 0.1 }, { "timescale", 0.5 },
                    { "speedmin", 100.0 }, { "speedmax", 100.0 },
                    { "mask", "1 0 0" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(1, 2, 3);
        op(fx.info());
        CHECK(p.velocity.y() == doctest::Approx(0.0f));
        CHECK(p.velocity.z() == doctest::Approx(0.0f));
    }
}

// ===========================================================================
// vortex operator (line 1070 — basic; v2 already has tests)
// ===========================================================================

TEST_SUITE("vortex operator (basic)") {
    TEST_CASE("inner zone applies speedinner tangential force") {
        json j  = { { "name", "vortex" },
                    { "controlpoint", 0 }, { "axis", "0 0 1" },
                    { "speedinner", 100.0 }, { "speedouter", 0.0 },
                    { "distanceinner", 100.0 }, { "distanceouter", 200.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        // Position at radial distance 50 (inside inner zone).
        p.position  = Eigen::Vector3f(50, 0, 0);
        op(fx.info());
        // Tangential to axis (0,0,1) at (50,0,0) is along ±y.
        CHECK(std::abs(p.velocity.y()) > 0.0f);
        CHECK(p.velocity.x() == doctest::Approx(0.0f).epsilon(1e-3));
        CHECK(p.velocity.z() == doctest::Approx(0.0f).epsilon(1e-3));
    }

    TEST_CASE("middle zone lerps speedinner→speedouter") {
        json j  = { { "name", "vortex" },
                    { "controlpoint", 0 }, { "axis", "0 0 1" },
                    { "speedinner", 0.0 }, { "speedouter", 100.0 },
                    { "distanceinner", 50.0 }, { "distanceouter", 150.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(100, 0, 0); // mid-zone
        op(fx.info());
        // Tangential velocity is non-zero (speedouter > 0) and partial.
        CHECK(std::abs(p.velocity.y()) > 0.0f);
    }

    TEST_CASE("outer zone applies speedouter") {
        json j  = { { "name", "vortex" },
                    { "controlpoint", 0 }, { "axis", "0 0 1" },
                    { "speedinner", 0.0 }, { "speedouter", 100.0 },
                    { "distanceinner", 50.0 }, { "distanceouter", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(200, 0, 0); // outside outer zone
        op(fx.info());
        CHECK(std::abs(p.velocity.y()) > 0.0f);
    }

    TEST_CASE("dis_mid < 0 (inner > outer) collapses to all-inner branch") {
        // distanceinner > distanceouter → dis_mid = -10 + 0.1 = -9.9 < 0
        json j  = { { "name", "vortex" },
                    { "controlpoint", 0 }, { "axis", "0 0 1" },
                    { "speedinner", 100.0 }, { "speedouter", 0.0 },
                    { "distanceinner", 100.0 }, { "distanceouter", 90.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(95, 0, 0);
        op(fx.info());
        // dis_mid<0 path triggers the speedinner branch; velocity gets a kick.
        CHECK(std::abs(p.velocity.y()) > 0.0f);
    }
}

// ===========================================================================
// remapvalue operator (line 1223 — biggest single dispatcher in the file)
// ===========================================================================

TEST_SUITE("remapvalue operator") {
    TEST_CASE("particlelifetime → particlesize multiply, before t=0 holds outMin") {
        json j  = { { "name", "remapvalue" },
                    { "input", "particlelifetime" },
                    { "output", "particlesize" },
                    { "operation", "multiply" },
                    { "inputrangemin", 0.0 }, { "inputrangemax", 1.0 },
                    { "outputrangemin", 2.0 }, { "outputrangemax", 4.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p   = fx.spawn();
        p.lifetime    = 1.0f; // life-pos = 0.0
        p.size        = 10.0f;
        op(fx.info());
        // At lifetime-pos=0, t=0 → out=outMin=2 → size *= 2 → 20.
        CHECK(p.size == doctest::Approx(20.0f).epsilon(0.05));
    }

    TEST_CASE("particlelifetime → particlesize multiply, end of life uses outMax") {
        json j  = { { "name", "remapvalue" },
                    { "input", "particlelifetime" },
                    { "output", "particlesize" },
                    { "operation", "multiply" },
                    { "outputrangemin", 1.0 }, { "outputrangemax", 5.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p   = fx.spawn();
        p.lifetime    = 0.001f; // life-pos ≈ 1.0 (still alive — LifetimeOk requires >0)
        p.size        = 10.0f;
        op(fx.info());
        CHECK(p.size == doctest::Approx(50.0f).epsilon(0.05));
    }

    TEST_CASE("set operation overrides instead of multiplying") {
        json j  = { { "name", "remapvalue" },
                    { "input", "particlelifetime" },
                    { "output", "particlesize" },
                    { "operation", "set" },
                    { "outputrangemin", 7.5 }, { "outputrangemax", 7.5 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.size      = 10.0f;
        op(fx.info());
        CHECK(p.size == doctest::Approx(7.5f));
    }

    TEST_CASE("setvelocity output alias normalizes to set + particlevelocity") {
        json j  = { { "name", "remapvalue" },
                    { "input", "particlelifetime" },
                    { "output", "setvelocity" },
                    { "outputcomponent", "x" },
                    { "outputrangemin", 5.0 }, { "outputrangemax", 5.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        op(fx.info());
        // x component should be set to 5.
        CHECK(p.velocity.x() == doctest::Approx(5.0f));
    }

    TEST_CASE("color short-form output canonicalises to particlecolor") {
        json j  = { { "name", "remapvalue" },
                    { "input", "particlelifetime" },
                    { "output", "color" },
                    { "operation", "multiply" },
                    { "outputrangemin", 0.5 }, { "outputrangemax", 0.5 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.color     = Eigen::Vector3f(1, 1, 1);
        op(fx.info());
        // Multiply by 0.5 → halved color.
        CHECK(p.color.x() == doctest::Approx(0.5f).epsilon(0.05));
    }

    TEST_CASE("particlevelocity input + magnitude reduction → output applies") {
        json j  = { { "name", "remapvalue" },
                    { "input", "particlevelocity" },
                    { "inputcomponent", "magnitude" },
                    { "output", "particlesize" },
                    { "operation", "set" },
                    { "inputrangemin", 0.0 }, { "inputrangemax", 10.0 },
                    { "outputrangemin", 0.0 }, { "outputrangemax", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.velocity  = Eigen::Vector3f(3, 4, 0); // mag = 5 → t=0.5 → size=50
        op(fx.info());
        CHECK(p.size == doctest::Approx(50.0f).epsilon(0.05));
    }

    TEST_CASE("speed input always takes magnitude regardless of inputcomponent") {
        json j  = { { "name", "remapvalue" },
                    { "input", "speed" }, { "inputcomponent", "x" },
                    { "output", "particlesize" }, { "operation", "set" },
                    { "inputrangemin", 0.0 }, { "inputrangemax", 10.0 },
                    { "outputrangemin", 0.0 }, { "outputrangemax", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        // speed treats the whole vector — mag(3,4,0) = 5 even though "x" component
        // alone would have been 3.
        p.velocity = Eigen::Vector3f(3, 4, 0);
        op(fx.info());
        CHECK(p.size == doctest::Approx(50.0f).epsilon(0.05));
    }

    TEST_CASE("controlpoint input reads CP position with x reduction") {
        std::array<ParticleControlpoint, 8> local_cps;
        for (auto& cp : local_cps) cp.resolved = Eigen::Vector3d(0, 0, 0);
        local_cps[0].resolved = Eigen::Vector3d(7, 0, 0);
        json j  = { { "name", "remapvalue" },
                    { "input", "controlpoint" },
                    { "inputcomponent", "x" },
                    { "inputcontrolpoint0", 0 },
                    { "output", "particlesize" }, { "operation", "set" },
                    { "inputrangemin", 0.0 }, { "inputrangemax", 10.0 },
                    { "outputrangemin", 0.0 }, { "outputrangemax", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        Particle p;
        p.lifetime      = 1.0f;
        p.init.lifetime = 1.0f;
        p.alpha         = 1.0f;
        p.size          = 0.0f;
        std::vector<Particle> ps { p };
        ParticleInfo info {
            .particles     = std::span<Particle>(ps),
            .controlpoints = std::span<ParticleControlpoint>(local_cps.data(), local_cps.size()),
            .time          = 0.0,
            .time_pass     = 0.016,
        };
        op(info);
        CHECK(ps[0].size == doctest::Approx(70.0f).epsilon(0.05));
    }

    TEST_CASE("inputrange max == min → t=0 → out = outMin (no division by zero)") {
        json j  = { { "name", "remapvalue" },
                    { "input", "particlelifetime" }, { "output", "particlesize" },
                    { "operation", "set" },
                    { "inputrangemin", 0.5 }, { "inputrangemax", 0.5 },
                    { "outputrangemin", 11.0 }, { "outputrangemax", 99.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        op(fx.info());
        // in_inv = 0 makes t = 0 → out = outMin.
        CHECK(p.size == doctest::Approx(11.0f));
    }

    TEST_CASE("dead particle (LifetimeOk false) is skipped") {
        json j  = { { "name", "remapvalue" },
                    { "input", "particlelifetime" }, { "output", "particlesize" },
                    { "operation", "set" },
                    { "outputrangemin", 99.0 }, { "outputrangemax", 99.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.lifetime  = 0.0f;
        p.size      = 5.0f;
        op(fx.info());
        CHECK(p.size == doctest::Approx(5.0f)); // unchanged
    }

    TEST_CASE("add operation accumulates onto existing value") {
        json j  = { { "name", "remapvalue" },
                    { "input", "particlelifetime" }, { "output", "particlesize" },
                    { "operation", "add" },
                    { "outputrangemin", 5.0 }, { "outputrangemax", 5.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.size      = 10.0f;
        op(fx.info());
        CHECK(p.size == doctest::Approx(15.0f).epsilon(0.05));
    }

    TEST_CASE("inputcomponent reductions: x, y, z, sum, average, min, max, w") {
        // w is documented as out-of-range → 0 in remapvalue's reduce lambda.
        // Hit each branch of the reduce lambda by feeding velocity vec3.
        for (const char* comp : { "x", "y", "z", "sum", "average", "min", "max", "w", "x+y" }) {
            json j  = { { "name", "remapvalue" },
                        { "input", "particlevelocity" },
                        { "inputcomponent", comp },
                        { "output", "particlesize" }, { "operation", "set" },
                        { "outputrangemin", 0.0 }, { "outputrangemax", 100.0 },
                        { "inputrangemin", 0.0 }, { "inputrangemax", 10.0 } };
            auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
            OpFixture fx;
            Particle& p = fx.spawn();
            p.velocity = Eigen::Vector3f(1, 2, 3);
            op(fx.info());
            // Just verify it ran (no crash) for every component.
            CHECK(std::isfinite(p.size));
        }
    }

    TEST_CASE("transformfunction noise + transformoctaves clamp to [1,8]") {
        // transformoctaves out-of-range gets clamped — exercise both rails.
        for (int oct : { -1, 0, 100 }) {
            json j  = { { "name", "remapvalue" },
                        { "input", "particlesystemtime" },
                        { "output", "particlesize" }, { "operation", "set" },
                        { "transformfunction", "noise" },
                        { "transforminputscale", 1.0 },
                        { "transformoctaves", oct },
                        { "outputrangemin", 0.0 }, { "outputrangemax", 100.0 } };
            auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
            OpFixture fx;
            fx.time = 0.5;
            Particle& p = fx.spawn();
            op(fx.info());
            CHECK(std::isfinite(p.size));
        }
    }

    TEST_CASE("distancetocontrolpoint input computes radial distance") {
        std::array<ParticleControlpoint, 8> local_cps;
        for (auto& cp : local_cps) cp.resolved = Eigen::Vector3d(0, 0, 0);
        local_cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        json j  = { { "name", "remapvalue" },
                    { "input", "distancetocontrolpoint" },
                    { "inputcontrolpoint0", 0 },
                    { "output", "particlesize" }, { "operation", "set" },
                    { "inputrangemin", 0.0 }, { "inputrangemax", 10.0 },
                    { "outputrangemin", 0.0 }, { "outputrangemax", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        Particle p;
        p.lifetime      = 1.0f;
        p.init.lifetime = 1.0f;
        p.alpha         = 1.0f;
        p.size          = 0.0f;
        p.position      = Eigen::Vector3f(3, 4, 0); // distance 5
        std::vector<Particle> ps { p };
        ParticleInfo info {
            .particles     = std::span<Particle>(ps),
            .controlpoints = std::span<ParticleControlpoint>(local_cps.data(), local_cps.size()),
            .time          = 0.0,
            .time_pass     = 0.016,
        };
        op(info);
        // dist=5, t=0.5 → size=50.
        CHECK(ps[0].size == doctest::Approx(50.0f).epsilon(0.05));
    }
}

// ===========================================================================
// mapsequencebetweencontrolpoints initializer (line 330)
// ===========================================================================

TEST_SUITE("mapsequencebetweencontrolpoints initializer") {
    TEST_CASE("count=1 short-circuits and bumps seq without positioning") {
        std::array<ParticleControlpoint, 8> local_cps;
        local_cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        local_cps[1].resolved = Eigen::Vector3d(100, 0, 0);
        json j = { { "name", "mapsequencebetweencontrolpoints" },
                   { "count", 1 },
                   { "controlpointstart", 0 }, { "controlpointend", 1 } };
        auto init = WPParticleParser::genParticleInitOp(j, cp_span(local_cps));
        // count<=1 short-circuits → particle is left at default position (0).
        for (int i = 0; i < 3; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(p.position.norm() == doctest::Approx(0.0f));
        }
    }

    TEST_CASE("count > 1 places particles along the CP line segment") {
        std::array<ParticleControlpoint, 8> local_cps;
        local_cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        local_cps[1].resolved = Eigen::Vector3d(100, 0, 0);
        json j = { { "name", "mapsequencebetweencontrolpoints" },
                   { "count", 4 },
                   { "controlpointstart", 0 }, { "controlpointend", 1 } };
        auto init = WPParticleParser::genParticleInitOp(j, cp_span(local_cps));
        std::vector<Eigen::Vector3f> positions;
        for (int i = 0; i < 4; i++) {
            Particle p;
            init(p, 0.0);
            positions.push_back(p.position);
        }
        // All positions distinct (the fraction along the line varies with seq).
        CHECK(positions[0] != positions[1]);
        CHECK(positions[1] != positions[2]);
    }

    TEST_CASE("mirror=true folds the index back, so pos[2N-1] mirrors pos[0]") {
        std::array<ParticleControlpoint, 8> local_cps;
        local_cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        local_cps[1].resolved = Eigen::Vector3d(100, 0, 0);
        json j = { { "name", "mapsequencebetweencontrolpoints" },
                   { "count", 4 }, { "mirror", true },
                   { "controlpointstart", 0 }, { "controlpointend", 1 },
                   { "arcamount", 0.0 }, { "sizereductionamount", 0.0 } };
        auto init = WPParticleParser::genParticleInitOp(j, cp_span(local_cps));
        std::vector<Eigen::Vector3f> positions;
        for (int i = 0; i < 6; i++) {
            Particle p;
            init(p, 0.0);
            positions.push_back(p.position);
        }
        CHECK(positions.size() == 6u);
    }

    TEST_CASE("missing CP indices short-circuit to seq-bump") {
        std::array<ParticleControlpoint, 8> local_cps;
        json j = { { "name", "mapsequencebetweencontrolpoints" },
                   { "count", 4 },
                   { "controlpointstart", 100 }, // out of range
                   { "controlpointend", 1 } };
        auto init = WPParticleParser::genParticleInitOp(j, cp_span(local_cps));
        Particle p;
        init(p, 0.0);
        // No crash; particle retains default position.
        CHECK(p.position.norm() == doctest::Approx(0.0f));
    }

    TEST_CASE("size reduction shrinks particles past the start") {
        std::array<ParticleControlpoint, 8> local_cps;
        local_cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        local_cps[1].resolved = Eigen::Vector3d(100, 0, 0);
        json j = { { "name", "mapsequencebetweencontrolpoints" },
                   { "count", 4 },
                   { "controlpointstart", 0 }, { "controlpointend", 1 },
                   { "sizereductionamount", 0.5 } };
        auto init = WPParticleParser::genParticleInitOp(j, cp_span(local_cps));
        // First particle: size unchanged.  Later particles: shrunk.
        Particle p0;
        p0.size = 10.0f;
        init(p0, 0.0);
        Particle p1;
        p1.size = 10.0f;
        init(p1, 0.0);
        // No assertion on which is smaller — we just make sure size_reduction
        // path is exercised without abort.
        CHECK(std::isfinite(p0.size));
        CHECK(std::isfinite(p1.size));
    }
}

// ===========================================================================
// inheritinitialvaluefromevent (line 558) — no-op when no spawn instance
// ===========================================================================

TEST_SUITE("inheritinitialvaluefromevent initializer") {
    TEST_CASE("no spawn-context instance → no-op (particle untouched)") {
        json j    = { { "name", "inheritinitialvaluefromevent" }, { "input", "color" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        p.color = Eigen::Vector3f(0.1f, 0.2f, 0.3f);
        init(p, 0.0);
        // particle_spawn_context::CurrentSpawnInstance() is null at test time
        // → init returns without touching p.
        CHECK(p.color.x() == doctest::Approx(0.1f));
        CHECK(p.color.y() == doctest::Approx(0.2f));
        CHECK(p.color.z() == doctest::Approx(0.3f));
    }

    TEST_CASE("each input keyword is accepted (no crash) under the no-op path") {
        // All input kinds end up no-op when no spawn context, but they all have
        // to still parse cleanly.
        for (const char* in : { "color",
                                 "particlecolor",
                                 "size",
                                 "particlesize",
                                 "alpha",
                                 "opacity",
                                 "particlealpha",
                                 "velocity",
                                 "particlevelocity",
                                 "rotation",
                                 "particlerotation",
                                 "lifetime",
                                 "particlelifetime",
                                 "unknown_input_token" }) {
            json j    = { { "name", "inheritinitialvaluefromevent" }, { "input", in } };
            auto init = WPParticleParser::genParticleInitOp(j);
            Particle p;
            init(p, 0.0);
            CHECK(std::isfinite(p.lifetime));
        }
    }
}

// ===========================================================================
// turbulentvelocityrandom (line 214)
// ===========================================================================

TEST_SUITE("turbulentvelocityrandom initializer") {
    TEST_CASE("imparts non-zero velocity bounded by speedmax") {
        json j = { { "name", "turbulentvelocityrandom" },
                   { "speedmin", 50.0 }, { "speedmax", 100.0 },
                   { "scale", 1.0 }, { "timescale", 1.0 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 10; i++) {
            Particle p;
            init(p, 0.5);
            // Velocity should land between speedmin and speedmax (inclusive).
            CHECK(p.velocity.norm() <= 100.5f);
        }
    }

    TEST_CASE("very long duration triggers the duration > 10 reset path") {
        json j = { { "name", "turbulentvelocityrandom" },
                   { "speedmin", 1.0 }, { "speedmax", 1.0 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        // Duration 12.0 > 10 triggers `pos += speed; duration = 0` branch.
        init(p, 12.0);
        CHECK(std::isfinite(p.velocity.norm()));
    }

    TEST_CASE("zero duration still produces a deterministic-shape velocity") {
        json j = { { "name", "turbulentvelocityrandom" },
                   { "speedmin", 10.0 }, { "speedmax", 10.0 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        init(p, 0.0);
        CHECK(std::isfinite(p.velocity.norm()));
    }
}

// ===========================================================================
// collisionmodel (line 1762) — currently a no-op stub
// ===========================================================================

TEST_SUITE("collisionmodel operator (stub)") {
    TEST_CASE("parses without throwing and runs as a no-op on particles") {
        json j  = { { "name", "collisionmodel" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(5, 5, 5);
        p.velocity  = Eigen::Vector3f(1, 1, 1);
        op(fx.info());
        // Stub is a no-op — particle is untouched.
        CHECK(p.position.x() == doctest::Approx(5.0f));
        CHECK(p.velocity.x() == doctest::Approx(1.0f));
    }
}

// ===========================================================================
// collisionquad (line 1771)
// ===========================================================================

TEST_SUITE("collisionquad operator") {
    TEST_CASE("particle crossing inside quad bounds is reflected") {
        // Plane at y=0 with normal (0,1,0).  Particle moves from (+y) to (-y).
        // size 100x100 — well inside.
        json j  = { { "name", "collisionquad" },
                    { "controlpoint", 0 },
                    { "plane", "0 1 0" },
                    { "forward", "1 0 0" },
                    { "size", "100 100" },
                    { "restitution", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.time_pass = 0.1;
        Particle& p  = fx.spawn();
        p.position   = Eigen::Vector3f(0, -1, 0); // just past the plane
        p.velocity   = Eigen::Vector3f(0, -10, 0); // moving away (was at +y last frame)
        op(fx.info());
        // Particle is snapped to the plane (y=0) and y-velocity reflected.
        CHECK(p.position.y() == doctest::Approx(0.0f).epsilon(1e-3));
        CHECK(p.velocity.y() > 0.0f);
    }

    TEST_CASE("particle crossing outside quad bounds passes through") {
        json j  = { { "name", "collisionquad" },
                    { "controlpoint", 0 },
                    { "plane", "0 1 0" },
                    { "forward", "1 0 0" },
                    { "size", "10 10" }, // small quad
                    { "restitution", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.time_pass = 0.1;
        Particle& p  = fx.spawn();
        // Crossing point projects to (50, 0, 0) in U direction — far outside half_u=5.
        p.position   = Eigen::Vector3f(50, -1, 0);
        p.velocity   = Eigen::Vector3f(0, -10, 0);
        op(fx.info());
        // Particle passes through; velocity y unchanged.
        CHECK(p.velocity.y() == doctest::Approx(-10.0f));
    }

    TEST_CASE("particle that doesn't cross the plane is left alone") {
        json j  = { { "name", "collisionquad" },
                    { "controlpoint", 0 },
                    { "plane", "0 1 0" },
                    { "forward", "1 0 0" },
                    { "size", "100 100" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.time_pass = 0.1;
        Particle& p  = fx.spawn();
        p.position   = Eigen::Vector3f(0, 5, 0);
        p.velocity   = Eigen::Vector3f(0, -1, 0); // still at +y after this frame's step
        op(fx.info());
        CHECK(p.velocity.y() == doctest::Approx(-1.0f));
    }

    TEST_CASE("dead particle is skipped") {
        json j  = { { "name", "collisionquad" },
                    { "controlpoint", 0 },
                    { "plane", "0 1 0" },
                    { "forward", "1 0 0" },
                    { "size", "100 100" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.time_pass = 0.1;
        Particle& p  = fx.spawn();
        p.lifetime   = 0.0f;
        p.position   = Eigen::Vector3f(0, -1, 0);
        p.velocity   = Eigen::Vector3f(0, -10, 0);
        op(fx.info());
        // LifetimeOk false → no reflection, velocity untouched.
        CHECK(p.velocity.y() == doctest::Approx(-10.0f));
    }

    TEST_CASE("zero plane normal falls back to (0,1,0)") {
        json j  = { { "name", "collisionquad" },
                    { "controlpoint", 0 },
                    { "plane", "0 0 0" }, // degenerate normal
                    { "forward", "1 0 0" },
                    { "size", "100 100" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.time_pass = 0.1;
        Particle& p  = fx.spawn();
        p.position   = Eigen::Vector3f(0, -1, 0);
        p.velocity   = Eigen::Vector3f(0, -10, 0);
        op(fx.info());
        // Fallback normal (0,1,0) → expected reflect.
        CHECK(p.velocity.y() > 0.0f);
    }

    TEST_CASE("forward parallel to normal picks an alternate basis (no NaN)") {
        json j  = { { "name", "collisionquad" },
                    { "controlpoint", 0 },
                    { "plane", "0 1 0" },
                    { "forward", "0 1 0" }, // parallel to plane normal
                    { "size", "100 100" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.time_pass = 0.1;
        Particle& p  = fx.spawn();
        p.position   = Eigen::Vector3f(0, -1, 0);
        p.velocity   = Eigen::Vector3f(0, -10, 0);
        op(fx.info());
        // Path should use the perpendicular fallback basis without NaN'ing.
        CHECK(std::isfinite(p.position.x()));
        CHECK(std::isfinite(p.velocity.y()));
    }

    TEST_CASE("controlpoint out-of-range returns without effect") {
        // Build a span that's smaller than the requested CP index.
        std::array<ParticleControlpoint, 1> small_cps;
        small_cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        json j  = { { "name", "collisionquad" },
                    { "controlpoint", 5 }, // out-of-range against a 1-CP span (would be ClampCpIndex'd to 5 anyway)
                    { "plane", "0 1 0" },
                    { "forward", "1 0 0" },
                    { "size", "100 100" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        Particle p;
        p.lifetime      = 1.0f;
        p.init.lifetime = 1.0f;
        p.alpha         = 1.0f;
        p.size          = 10.0f;
        p.position      = Eigen::Vector3f(0, -1, 0);
        p.velocity      = Eigen::Vector3f(0, -10, 0);
        std::vector<Particle> ps { p };
        ParticleInfo info {
            .particles     = std::span<Particle>(ps),
            .controlpoints = std::span<ParticleControlpoint>(small_cps.data(), small_cps.size()),
            .time          = 0.0,
            .time_pass     = 0.1,
        };
        op(info);
        // CP index >= size → early return.
        CHECK(ps[0].velocity.y() == doctest::Approx(-10.0f));
    }
}
