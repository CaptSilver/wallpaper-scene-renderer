#include <doctest.h>

#include "WPParticleParser.hpp"
#include "Particle/Particle.h"
#include "Particle/ParticleEmitter.h"
#include "Particle/BlendWindow.h"
#include "Particle/HsvColor.h"
#include "Particle/ParticleCollision.h"
#include "wpscene/WPParticleObject.h"

#include <nlohmann/json.hpp>
#include <Eigen/Core>
#include <vector>
#include <array>
#include <cmath>

using namespace wallpaper;
using nlohmann::json;

namespace
{
// Build a ParticleInfo around the supplied particle vector with default
// (zero-resolved) controlpoints unless overridden.
struct OpFixture {
    std::vector<Particle>             particles;
    std::array<ParticleControlpoint, 8> cps;
    double                            time { 0.0 };
    double                            time_pass { 0.016 };

    ParticleInfo info() {
        return ParticleInfo {
            .particles     = std::span<Particle>(particles),
            .controlpoints = std::span<const ParticleControlpoint>(cps.data(), cps.size()),
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
} // namespace

// ===========================================================================
// BlendWindow — pure helper.
// ===========================================================================

TEST_SUITE("BlendWindow") {
    TEST_CASE("absent fields → trivial: factor is always 1") {
        json j = json::object();
        auto bw = BlendWindow::FromJson(j);
        CHECK_FALSE(bw.Has());
        CHECK(bw.Factor(0.0) == doctest::Approx(1.0));
        CHECK(bw.Factor(0.5) == doctest::Approx(1.0));
        CHECK(bw.Factor(1.0) == doctest::Approx(1.0));
    }

    TEST_CASE("fade-in only ramps from 0 to 1 across the window") {
        json j = { { "blendinstart", 0.0 }, { "blendinend", 0.2 } };
        auto bw = BlendWindow::FromJson(j);
        CHECK(bw.Has());
        CHECK(bw.Factor(0.0) == doctest::Approx(0.0));
        CHECK(bw.Factor(0.1) == doctest::Approx(0.5));
        CHECK(bw.Factor(0.2) == doctest::Approx(1.0));
        CHECK(bw.Factor(0.5) == doctest::Approx(1.0));
        CHECK(bw.Factor(1.0) == doctest::Approx(1.0));
    }

    TEST_CASE("fade-out only ramps from 1 to 0 across the window") {
        json j = { { "blendoutstart", 0.8 }, { "blendoutend", 1.0 } };
        auto bw = BlendWindow::FromJson(j);
        CHECK(bw.Has());
        CHECK(bw.Factor(0.0) == doctest::Approx(1.0));
        CHECK(bw.Factor(0.8) == doctest::Approx(1.0));
        CHECK(bw.Factor(0.9) == doctest::Approx(0.5));
        CHECK(bw.Factor(1.0) == doctest::Approx(0.0));
    }

    TEST_CASE("fade-in then fade-out — full pipeline") {
        json j = { { "blendinstart", 0.0 }, { "blendinend", 0.2 },
                   { "blendoutstart", 0.8 }, { "blendoutend", 1.0 } };
        auto bw = BlendWindow::FromJson(j);
        CHECK(bw.Has());
        CHECK(bw.Factor(0.0) == doctest::Approx(0.0));
        CHECK(bw.Factor(0.5) == doctest::Approx(1.0));
        CHECK(bw.Factor(1.0) == doctest::Approx(0.0));
    }

    TEST_CASE("degenerate window (start==end) does not divide by zero") {
        json j = { { "blendinstart", 0.5 }, { "blendinend", 0.5 } };
        auto bw = BlendWindow::FromJson(j);
        CHECK_FALSE(bw.has_fade_in); // collapsed to 0-width — gated off
        CHECK(bw.Factor(0.5) == doctest::Approx(1.0));
    }
}

// ===========================================================================
// HsvColor — round-trip + edge cases.
// ===========================================================================

TEST_SUITE("HsvColor") {
    TEST_CASE("HSV→RGB primaries land on the right hue") {
        // Pure red at h=0, s=1, v=1
        auto rgb = HsvToRgb(0.0, 1.0, 1.0);
        CHECK(rgb.x() == doctest::Approx(1.0));
        CHECK(rgb.y() == doctest::Approx(0.0));
        CHECK(rgb.z() == doctest::Approx(0.0));
        // Pure green at h=120
        rgb = HsvToRgb(120.0, 1.0, 1.0);
        CHECK(rgb.x() == doctest::Approx(0.0));
        CHECK(rgb.y() == doctest::Approx(1.0));
        CHECK(rgb.z() == doctest::Approx(0.0));
        // Pure blue at h=240
        rgb = HsvToRgb(240.0, 1.0, 1.0);
        CHECK(rgb.x() == doctest::Approx(0.0));
        CHECK(rgb.y() == doctest::Approx(0.0));
        CHECK(rgb.z() == doctest::Approx(1.0));
    }

    TEST_CASE("hue wraps modulo 360") {
        auto a = HsvToRgb(0.0, 1.0, 1.0);
        auto b = HsvToRgb(360.0, 1.0, 1.0);
        auto c = HsvToRgb(720.0, 1.0, 1.0);
        CHECK(a.isApprox(b));
        CHECK(a.isApprox(c));
    }

    TEST_CASE("zero saturation → gray regardless of hue") {
        auto rgb = HsvToRgb(45.0, 0.0, 0.5);
        CHECK(rgb.x() == doctest::Approx(0.5));
        CHECK(rgb.y() == doctest::Approx(0.5));
        CHECK(rgb.z() == doctest::Approx(0.5));
    }

    TEST_CASE("RGB→HSV→RGB round-trips for saturated colors") {
        for (double h : { 30.0, 90.0, 150.0, 210.0, 270.0, 330.0 }) {
            auto rgb1 = HsvToRgb(h, 0.7, 0.8);
            auto hsv  = RgbToHsv(rgb1.x(), rgb1.y(), rgb1.z());
            auto rgb2 = HsvToRgb(hsv.x(), hsv.y(), hsv.z());
            CHECK(rgb1.isApprox(rgb2, 1e-6));
        }
    }

    TEST_CASE("clamps over-bright value") {
        auto rgb = HsvToRgb(0.0, 1.0, 2.0); // v > 1 → clamps to 1
        CHECK(rgb.x() == doctest::Approx(1.0));
    }
}

// ===========================================================================
// ParticleCollision::ReflectVelocity
// ===========================================================================

TEST_SUITE("ParticleCollision") {
    TEST_CASE("Reflect inverts the normal component for a particle moving into the surface") {
        Particle p;
        p.velocity = Eigen::Vector3f(1, -1, 0);
        Eigen::Vector3d n(0, 1, 0);
        ParticleCollision::ReflectVelocity(p, n);
        CHECK(p.velocity.x() == doctest::Approx(1.0));
        CHECK(p.velocity.y() == doctest::Approx(1.0));
        CHECK(p.velocity.z() == doctest::Approx(0.0));
    }

    TEST_CASE("Reflect leaves a particle moving away from the surface alone") {
        Particle p;
        p.velocity = Eigen::Vector3f(1, 1, 0);
        Eigen::Vector3d n(0, 1, 0);
        ParticleCollision::ReflectVelocity(p, n);
        CHECK(p.velocity.y() == doctest::Approx(1.0)); // unchanged
    }

    TEST_CASE("Restitution > 1 amplifies the bounce") {
        Particle p;
        p.velocity = Eigen::Vector3f(0, -2, 0);
        Eigen::Vector3d n(0, 1, 0);
        ParticleCollision::ReflectVelocity(p, n, 2.0);
        // v_new = v - (1+2)*(v.n)*n = (0,-2,0) - 3*(-2)*(0,1,0) = (0, 4, 0)
        CHECK(p.velocity.y() == doctest::Approx(4.0));
    }
}

// ===========================================================================
// capvelocity
// ===========================================================================

TEST_SUITE("capvelocity") {
    TEST_CASE("clamps fast particles to maxspeed") {
        json j = { { "name", "capvelocity" }, { "maxspeed", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.velocity = Eigen::Vector3f(300, 0, 0);
        op(fx.info());
        CHECK(p.velocity.norm() == doctest::Approx(100.0));
    }

    TEST_CASE("leaves slower particles alone") {
        json j = { { "name", "capvelocity" }, { "maxspeed", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.velocity = Eigen::Vector3f(50, 0, 0);
        op(fx.info());
        CHECK(p.velocity.x() == doctest::Approx(50.0));
    }

    TEST_CASE("dead particles are skipped") {
        json j = { { "name", "capvelocity" }, { "maxspeed", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.lifetime = 0.0f;
        p.velocity = Eigen::Vector3f(500, 0, 0);
        op(fx.info());
        CHECK(p.velocity.x() == doctest::Approx(500.0));
    }
}

// ===========================================================================
// hsvcolorrandom
// ===========================================================================

TEST_SUITE("hsvcolorrandom initializer") {
    TEST_CASE("produces RGB color in [0, 1] range") {
        json j = { { "name", "hsvcolorrandom" },
                   { "huemin", 0.0 }, { "huemax", 360.0 },
                   { "saturationmin", 0.5 }, { "saturationmax", 1.0 },
                   { "valuemin", 1.0 }, { "valuemax", 1.0 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        for (int i = 0; i < 50; i++) {
            init(p, 0.0);
            CHECK(p.color.x() >= 0.0f);
            CHECK(p.color.x() <= 1.0f);
            CHECK(p.color.y() >= 0.0f);
            CHECK(p.color.y() <= 1.0f);
            CHECK(p.color.z() >= 0.0f);
            CHECK(p.color.z() <= 1.0f);
        }
    }

    TEST_CASE("zero saturation → grayscale particle") {
        json j = { { "name", "hsvcolorrandom" },
                   { "saturationmin", 0.0 }, { "saturationmax", 0.0 },
                   { "valuemin", 0.5 }, { "valuemax", 0.5 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(0.5));
        CHECK(p.color.y() == doctest::Approx(0.5));
        CHECK(p.color.z() == doctest::Approx(0.5));
    }
}

// ===========================================================================
// colorlist
// ===========================================================================

TEST_SUITE("colorlist initializer") {
    TEST_CASE("picks one of the listed colors") {
        json j = { { "name", "colorlist" },
                   { "colors", { "1.0 0.0 0.0", "0.0 1.0 0.0", "0.0 0.0 1.0" } } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 30; i++) {
            Particle p;
            init(p, 0.0);
            int channels_lit = (p.color.x() > 0.5f) + (p.color.y() > 0.5f) + (p.color.z() > 0.5f);
            CHECK(channels_lit == 1); // exactly one primary
        }
    }

    TEST_CASE("normalizes 0..255 strings to 0..1") {
        json j = { { "name", "colorlist" }, { "colors", { "255 0 0" } } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(1.0));
        CHECK(p.color.y() == doctest::Approx(0.0));
    }

    TEST_CASE("empty colors array does not crash — falls back to white") {
        json j = { { "name", "colorlist" }, { "colors", json::array() } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(1.0));
        CHECK(p.color.y() == doctest::Approx(1.0));
        CHECK(p.color.z() == doctest::Approx(1.0));
    }
}

// ===========================================================================
// maintaindistancetocontrolpoint
// ===========================================================================

TEST_SUITE("maintaindistancetocontrolpoint") {
    TEST_CASE("hard clamp snaps the particle onto the sphere surface") {
        json j = { { "name", "maintaindistancetocontrolpoint" },
                   { "controlpoint", 0 }, { "distance", 100.0 },
                   { "variablestrength", 0.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position = Eigen::Vector3f(50, 0, 0); // inside
        op(fx.info());
        CHECK(p.position.norm() == doctest::Approx(100.0));
    }

    TEST_CASE("particle outside the sphere is pushed back to surface") {
        json j = { { "name", "maintaindistancetocontrolpoint" },
                   { "controlpoint", 0 }, { "distance", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position = Eigen::Vector3f(200, 0, 0);
        op(fx.info());
        CHECK(p.position.x() == doctest::Approx(100.0));
    }

    TEST_CASE("zero CP, zero position → no NaN (epsilon guard)") {
        json j = { { "name", "maintaindistancetocontrolpoint" },
                   { "controlpoint", 0 }, { "distance", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position = Eigen::Vector3f(0, 0, 0);
        op(fx.info());
        CHECK(std::isfinite(p.position.x()));
    }
}

// ===========================================================================
// collisionplane
// ===========================================================================

TEST_SUITE("collisionplane") {
    TEST_CASE("particle below the y=0 plane is snapped to the plane") {
        json j = { { "name", "collisionplane" },
                   { "plane", { 0.0, 1.0, 0.0, 0.0 } },
                   { "controlpoint", 0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position = Eigen::Vector3f(10, -5, 0);
        p.velocity = Eigen::Vector3f(0, -3, 0);
        op(fx.info());
        CHECK(p.position.y() == doctest::Approx(0.0));
        CHECK(p.velocity.y() == doctest::Approx(3.0)); // reflected
    }

    TEST_CASE("particle above the plane is left alone") {
        json j = { { "name", "collisionplane" },
                   { "plane", { 0.0, 1.0, 0.0, 0.0 } },
                   { "controlpoint", 0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position = Eigen::Vector3f(0, 10, 0);
        p.velocity = Eigen::Vector3f(1, 1, 0);
        op(fx.info());
        CHECK(p.position.y() == doctest::Approx(10.0));
        CHECK(p.velocity.y() == doctest::Approx(1.0));
    }

    TEST_CASE("plane shifted by `distance` along +normal raises the collision surface") {
        // Plane (normal=+y, d=0) shifted by distance=5 along the normal lands
        // at y=+5: a particle at y=-3 is now BELOW the plane and is snapped
        // upward.
        json j = { { "name", "collisionplane" },
                   { "plane", { 0.0, 1.0, 0.0, 0.0 } },
                   { "distance", 5.0 },
                   { "controlpoint", 0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position = Eigen::Vector3f(0, -3, 0);
        p.velocity = Eigen::Vector3f(0, -1, 0);
        op(fx.info());
        CHECK(p.position.y() == doctest::Approx(5.0));
        CHECK(p.velocity.y() == doctest::Approx(1.0)); // reflected
    }
}

// ===========================================================================
// collisionsphere
// ===========================================================================

TEST_SUITE("collisionsphere") {
    TEST_CASE("particle inside sphere is pushed to surface and reflected") {
        json j = { { "name", "collisionsphere" },
                   { "origin", { 0.0, 0.0, 0.0 } },
                   { "radius", 100.0 },
                   { "controlpoint", 0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position = Eigen::Vector3f(50, 0, 0); // inside, radius 50
        p.velocity = Eigen::Vector3f(-10, 0, 0); // moving toward center (into surface)
        op(fx.info());
        CHECK(p.position.norm() == doctest::Approx(100.0));
        CHECK(p.velocity.x() == doctest::Approx(10.0)); // reflected
    }

    TEST_CASE("particle outside sphere is left alone") {
        json j = { { "name", "collisionsphere" }, { "radius", 50.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position = Eigen::Vector3f(200, 0, 0);
        p.velocity = Eigen::Vector3f(1, 0, 0);
        op(fx.info());
        CHECK(p.position.x() == doctest::Approx(200.0));
    }
}

// ===========================================================================
// boids
// ===========================================================================

TEST_SUITE("boids") {
    TEST_CASE("two particles within neighbor distance steer toward each other (cohesion)") {
        json j = { { "name", "boids" },
                   { "neighborthreshold", 100.0 },
                   { "separationthreshold", 0.0 }, // disable separation
                   { "cohesionfactor", 1.0 },
                   { "alignmentfactor", 0.0 },
                   { "separationfactor", 0.0 },
                   { "maxspeed", 1000.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.spawn().position = Eigen::Vector3f(-10, 0, 0);
        fx.spawn().position = Eigen::Vector3f(10, 0, 0);
        op(fx.info());
        // First particle should accelerate toward +x (toward neighbour).
        CHECK(fx.particles[0].velocity.x() > 0.0f);
        CHECK(fx.particles[1].velocity.x() < 0.0f);
    }

    TEST_CASE("speed cap clips post-acceleration velocity") {
        json j = { { "name", "boids" }, { "maxspeed", 5.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.velocity = Eigen::Vector3f(100, 0, 0);
        op(fx.info());
        CHECK(p.velocity.norm() <= 5.0f + 1e-3f);
    }
}

// ===========================================================================
// remapvalue (universal mapping)
// ===========================================================================

TEST_SUITE("remapvalue") {
    TEST_CASE("particlelifetime → particlealpha multiply (legacy 'opacity' alias)") {
        json j = { { "name", "remapvalue" },
                   { "operation", "multiply" },
                   { "input", "particlelifetime" },
                   { "output", "opacity" },
                   { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.lifetime      = 0.5f;
        p.init.lifetime = 1.0f;
        p.alpha         = 1.0f;
        op(fx.info());
        // lifetime pos = 0.5 → linear → mapped 0.5 → alpha *= 0.5
        CHECK(p.alpha == doctest::Approx(0.5));
    }

    TEST_CASE("set operation overrides instead of multiplying") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "particlelifetime" },
                   { "output", "particlesize" },
                   { "outputrangemin", 100.0 }, { "outputrangemax", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.size = 20.0f;
        op(fx.info());
        CHECK(p.size == doctest::Approx(100.0));
    }

    TEST_CASE("add operation accumulates onto current value") {
        json j = { { "name", "remapvalue" },
                   { "operation", "add" },
                   { "input", "particlelifetime" },
                   { "output", "particlesize" },
                   { "outputrangemin", 5.0 }, { "outputrangemax", 5.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.size = 20.0f;
        op(fx.info());
        CHECK(p.size == doctest::Approx(25.0));
    }

    TEST_CASE("input range normalisation and output range mapping") {
        // Input in [0, 100]; output in [10, 30].  Time = 50 → t=0.5 → out=20.
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "particlesystemtime" },
                   { "output", "particlesize" },
                   { "inputrangemin", 0.0 }, { "inputrangemax", 100.0 },
                   { "outputrangemin", 10.0 }, { "outputrangemax", 30.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.time = 50.0;
        Particle& p = fx.spawn();
        op(fx.info());
        CHECK(p.size == doctest::Approx(20.0));
    }

    TEST_CASE("smoothstep transform is non-linear at the midpoint") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "particlelifetime" },
                   { "output", "particlesize" },
                   { "transformfunction", "smoothstep" },
                   { "outputrangemin", 0.0 }, { "outputrangemax", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        // life = 0.25 → smoothstep(0.25) = 3*0.0625 - 2*0.015625 = 0.15625
        p.lifetime      = 0.75f; // life-pos = 1 - 0.75 = 0.25
        p.init.lifetime = 1.0f;
        op(fx.info());
        CHECK(p.size == doctest::Approx(15.625).epsilon(0.01));
    }

    TEST_CASE("controlpoint input reads CP position component") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "controlpoint" },
                   { "inputcomponent", "x" },
                   { "output", "particlesize" },
                   { "inputcontrolpoint0", 0 },
                   { "inputrangemin", 0.0 }, { "inputrangemax", 100.0 },
                   { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.cps[0].resolved = Eigen::Vector3d(50, 0, 0);
        Particle& p = fx.spawn();
        op(fx.info());
        CHECK(p.size == doctest::Approx(0.5));
    }

    TEST_CASE("input: random produces a stable per-particle value") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "random" },
                   { "output", "particlealpha" },
                   { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        op(fx.info());
        float a1 = p.alpha;
        // Run again with the SAME particle/seed — the value must not change.
        op(fx.info());
        float a2 = p.alpha;
        CHECK(a1 == doctest::Approx(a2));
    }
}

// ===========================================================================
// inheritcontrolpointvelocity initializer
// ===========================================================================

TEST_SUITE("inheritcontrolpointvelocity") {
    TEST_CASE("adds CP velocity to particle initial velocity") {
        std::array<ParticleControlpoint, 8> cps;
        cps[0].velocity = Eigen::Vector3d(50, 0, 0);
        json j = { { "name", "inheritcontrolpointvelocity" },
                   { "controlpoint", 0 },
                   { "min", 1.0 }, { "max", 1.0 } };
        auto init = WPParticleParser::genParticleInitOp(
            j, std::span<const ParticleControlpoint>(cps.data(), cps.size()));
        Particle p;
        p.velocity = Eigen::Vector3f(0, 0, 0);
        init(p, 0.0);
        CHECK(p.velocity.x() == doctest::Approx(50.0));
    }

    TEST_CASE("scales by random in [min, max]") {
        std::array<ParticleControlpoint, 8> cps;
        cps[0].velocity = Eigen::Vector3d(100, 0, 0);
        json j = { { "name", "inheritcontrolpointvelocity" },
                   { "controlpoint", 0 },
                   { "min", 0.5 }, { "max", 0.5 } };
        auto init = WPParticleParser::genParticleInitOp(
            j, std::span<const ParticleControlpoint>(cps.data(), cps.size()));
        Particle p;
        p.velocity = Eigen::Vector3f(0, 0, 0);
        init(p, 0.0);
        CHECK(p.velocity.x() == doctest::Approx(50.0));
    }
}

// ===========================================================================
// Particle::RandomFloat — stable per-seed value.
// ===========================================================================

TEST_SUITE("Particle.RandomFloat") {
    TEST_CASE("same seed → same float") {
        Particle a, b;
        a.random_seed = b.random_seed = 0x12345678u;
        CHECK(a.RandomFloat() == doctest::Approx(b.RandomFloat()));
    }

    TEST_CASE("different seeds → different floats (statistical, not guaranteed)") {
        Particle a, b;
        a.random_seed = 1u;
        b.random_seed = 0xffffffffu;
        CHECK(a.RandomFloat() != b.RandomFloat());
    }

    TEST_CASE("output is in [0, 1)") {
        for (uint32_t s = 1; s < 10000; s += 137) {
            Particle p;
            p.random_seed = s;
            float v = p.RandomFloat();
            CHECK(v >= 0.0f);
            CHECK(v < 1.0f);
        }
    }
}
