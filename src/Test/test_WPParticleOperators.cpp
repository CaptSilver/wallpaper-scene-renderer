#include <doctest.h>

#include "WPParticleParser.hpp"
#include "Particle/Particle.h"
#include "Particle/ParticleEmitter.h"
#include "Particle/ParticleSystem.h"
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

    TEST_CASE("subtract operation reduces current value") {
        json j = { { "name", "remapvalue" },
                   { "operation", "subtract" },
                   { "input", "particlelifetime" },
                   { "output", "particlesize" },
                   { "outputrangemin", 5.0 }, { "outputrangemax", 5.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.size = 20.0f;
        op(fx.info());
        CHECK(p.size == doctest::Approx(15.0));
    }

    TEST_CASE("`remap` is the explicit replace operation (alias for set)") {
        json j_remap = { { "name", "remapvalue" },
                         { "operation", "remap" },
                         { "input", "particlelifetime" },
                         { "output", "particlesize" },
                         { "outputrangemin", 77.0 }, { "outputrangemax", 77.0 } };
        json j_set   = { { "name", "remapvalue" },
                         { "operation", "set" },
                         { "input", "particlelifetime" },
                         { "output", "particlesize" },
                         { "outputrangemin", 77.0 }, { "outputrangemax", 77.0 } };
        auto op_r = WPParticleParser::genParticleOperatorOp(j_remap, empty_override());
        auto op_s = WPParticleParser::genParticleOperatorOp(j_set, empty_override());
        OpFixture fx_r, fx_s;
        Particle& p_r = fx_r.spawn();
        Particle& p_s = fx_s.spawn();
        p_r.size = p_s.size = 5.0f;
        op_r(fx_r.info());
        op_s(fx_s.info());
        CHECK(p_r.size == doctest::Approx(p_s.size));
        CHECK(p_r.size == doctest::Approx(77.0));
    }

    TEST_CASE("square transform alternates 0 and 1 at the half-period") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "particlesystemtime" },
                   { "output", "particlesize" },
                   { "transformfunction", "square" },
                   { "transforminputscale", 1.0 },
                   { "inputrangemin", 0.0 }, { "inputrangemax", 1.0 },
                   { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        // t=0.25 → sin(π/2)=1 ≥ 0 → high
        fx.time = 0.25; op(fx.info());
        CHECK(p.size == doctest::Approx(1.0));
        // t=0.75 → sin(3π/2)=-1 → low
        fx.time = 0.75; op(fx.info());
        CHECK(p.size == doctest::Approx(0.0));
    }

    TEST_CASE("saw transform period is 1 (fractional part)") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "particlesystemtime" },
                   { "output", "particlesize" },
                   { "transformfunction", "saw" },
                   { "inputrangemin", 0.0 }, { "inputrangemax", 1.0 },
                   { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        fx.time = 0.3; op(fx.info());
        CHECK(p.size == doctest::Approx(0.3));
    }

    TEST_CASE("triangle wave: |2*fract(x)-1| peaks at boundaries, zero at midpoint") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "particlesystemtime" },
                   { "output", "particlesize" },
                   { "transformfunction", "triangle" },
                   { "inputrangemin", 0.0 }, { "inputrangemax", 1.0 },
                   { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        fx.time = 0.0; op(fx.info());
        CHECK(p.size == doctest::Approx(1.0));  // |2*0 - 1| = 1
        fx.time = 0.5; op(fx.info());
        CHECK(p.size == doctest::Approx(0.0));  // |2*0.5 - 1| = 0
        fx.time = 0.25; op(fx.info());
        CHECK(p.size == doctest::Approx(0.5));  // |2*0.25 - 1| = 0.5
    }

    TEST_CASE("simplexnoise stays in [0, 1] across many samples") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "particlesystemtime" },
                   { "output", "particlesize" },
                   { "transformfunction", "simplexnoise" },
                   { "inputrangemin", 0.0 }, { "inputrangemax", 100.0 },
                   { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        for (double t = 0.0; t < 100.0; t += 1.7) {
            fx.time = t; op(fx.info());
            CHECK(p.size >= 0.0f);
            CHECK(p.size <= 1.0f);
        }
    }

    TEST_CASE("fbmnoise reduces to simplexnoise when octaves=1") {
        json j_simp = { { "name", "remapvalue" },
                        { "operation", "set" },
                        { "input", "particlesystemtime" },
                        { "output", "particlesize" },
                        { "transformfunction", "simplexnoise" },
                        { "inputrangemin", 0.0 }, { "inputrangemax", 1.0 },
                        { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        json j_fbm  = j_simp;
        j_fbm["transformfunction"] = "fbmnoise";
        j_fbm["transformoctaves"] = 1;
        auto op_s = WPParticleParser::genParticleOperatorOp(j_simp, empty_override());
        auto op_f = WPParticleParser::genParticleOperatorOp(j_fbm, empty_override());
        OpFixture fx_s, fx_f;
        fx_s.time = fx_f.time = 0.42;
        Particle& p_s = fx_s.spawn();
        Particle& p_f = fx_f.spawn();
        op_s(fx_s.info());
        op_f(fx_f.info());
        CHECK(p_s.size == doctest::Approx(p_f.size).epsilon(0.01));
    }

    TEST_CASE("input: noise produces a value in [0, 1] that varies with position") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "noise" },
                   { "output", "particlealpha" },
                   { "inputrangemin", 0.0 }, { "inputrangemax", 1.0 },
                   { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.particles.resize(2);
        for (auto& q : fx.particles) {
            q.lifetime = 1.0f;
            q.init.lifetime = 1.0f;
        }
        fx.particles[0].position = Eigen::Vector3f(33, 17, 0);
        fx.particles[1].position = Eigen::Vector3f(540, 215, 0);
        op(fx.info());
        for (const auto& q : fx.particles) {
            CHECK(q.alpha >= 0.0f);
            CHECK(q.alpha <= 1.0f);
        }
        // Far-apart particles produce different noise samples (statistical, not
        // guaranteed at every position pair, but Perlin noise at this distance
        // is virtually never identical).
        CHECK(fx.particles[0].alpha != fx.particles[1].alpha);
    }

    TEST_CASE("input: noise honours transformoctaves for fbm summing") {
        json j_one = { { "name", "remapvalue" },
                       { "operation", "set" },
                       { "input", "noise" },
                       { "output", "particlealpha" },
                       { "transformoctaves", 1 },
                       { "inputrangemin", 0.0 }, { "inputrangemax", 1.0 },
                       { "outputrangemin", 0.0 }, { "outputrangemax", 1.0 } };
        json j_four = j_one;
        j_four["transformoctaves"] = 4;
        auto op_o = WPParticleParser::genParticleOperatorOp(j_one, empty_override());
        auto op_f = WPParticleParser::genParticleOperatorOp(j_four, empty_override());
        OpFixture fx_o, fx_f;
        Particle& p_o = fx_o.spawn();
        Particle& p_f = fx_f.spawn();
        p_o.position = Eigen::Vector3f(123, 45, 0);
        p_f.position = Eigen::Vector3f(123, 45, 0);
        op_o(fx_o.info());
        op_f(fx_f.info());
        CHECK(p_o.alpha >= 0.0f);
        CHECK(p_o.alpha <= 1.0f);
        CHECK(p_f.alpha >= 0.0f);
        CHECK(p_f.alpha <= 1.0f);
        // Different octave counts almost always produce different sums.
        CHECK(p_o.alpha != p_f.alpha);
    }

    TEST_CASE("output: controlpoint writes back to the resolved CP") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "particlelifetime" },
                   { "output", "controlpoint" },
                   { "outputcontrolpoint0", 3 },
                   { "outputcomponent", "x" },
                   { "outputrangemin", 50.0 }, { "outputrangemax", 50.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.cps[3].resolved = Eigen::Vector3d(0, 0, 0);
        fx.spawn();
        op(fx.info());
        CHECK(fx.cps[3].resolved.x() == doctest::Approx(50.0));
        CHECK(fx.cps[3].resolved.y() == doctest::Approx(0.0));
    }

    TEST_CASE("output: controlpoint with no outputcomponent broadcasts scalar to xyz") {
        json j = { { "name", "remapvalue" },
                   { "operation", "set" },
                   { "input", "particlelifetime" },
                   { "output", "controlpoint" },
                   { "outputcontrolpoint0", 5 },
                   { "outputrangemin", 7.0 }, { "outputrangemax", 7.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.spawn();
        op(fx.info());
        CHECK(fx.cps[5].resolved.x() == doctest::Approx(7.0));
        CHECK(fx.cps[5].resolved.y() == doctest::Approx(7.0));
        CHECK(fx.cps[5].resolved.z() == doctest::Approx(7.0));
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
// Universal blend-window coverage on existing kinematic operators
// ===========================================================================

TEST_SUITE("BlendWindow on kinematic ops") {
    TEST_CASE("movement: gravity is scaled by the blend factor") {
        json j_no_blend = { { "name", "movement" },
                            { "gravity", "0 -10 0" } };
        json j_blend    = { { "name", "movement" },
                            { "gravity", "0 -10 0" },
                            { "blendinstart", 0.0 }, { "blendinend", 1.0 } };
        wpscene::ParticleInstanceoverride over;
        over.enabled = true;
        over.speed   = 1.0f;
        auto op_a = WPParticleParser::genParticleOperatorOp(j_no_blend, over);
        auto op_b = WPParticleParser::genParticleOperatorOp(j_blend, over);
        OpFixture fx_a, fx_b;
        Particle& p_a = fx_a.spawn();
        Particle& p_b = fx_b.spawn();
        // Both particles at LifetimePos = 0.5; with blend window [0,1] the
        // blend factor at life=0.5 is exactly 0.5.
        p_a.lifetime = p_b.lifetime = 0.5f;
        p_a.init.lifetime = p_b.init.lifetime = 1.0f;
        op_a(fx_a.info());
        op_b(fx_b.info());
        // No-blend version applies full gravity; blended version applies half.
        CHECK(p_a.velocity.y() < p_b.velocity.y());
        CHECK(p_b.velocity.y() == doctest::Approx(p_a.velocity.y() * 0.5));
    }

    TEST_CASE("controlpointattract: force is scaled by the blend factor") {
        json j_no_blend = { { "name", "controlpointattract" },
                            { "controlpoint", 0 },
                            { "scale", 50.0 },
                            { "threshold", 1000.0 } };
        json j_blend    = { { "name", "controlpointattract" },
                            { "controlpoint", 0 },
                            { "scale", 50.0 },
                            { "threshold", 1000.0 },
                            { "blendinstart", 0.0 }, { "blendinend", 1.0 } };
        auto op_a = WPParticleParser::genParticleOperatorOp(j_no_blend, empty_override());
        auto op_b = WPParticleParser::genParticleOperatorOp(j_blend, empty_override());
        OpFixture fx_a, fx_b;
        fx_a.cps[0].resolved = fx_b.cps[0].resolved = Eigen::Vector3d(100, 0, 0);
        Particle& p_a = fx_a.spawn();
        Particle& p_b = fx_b.spawn();
        p_a.lifetime = p_b.lifetime = 0.5f;
        p_a.init.lifetime = p_b.init.lifetime = 1.0f;
        op_a(fx_a.info());
        op_b(fx_b.info());
        // Blended version is half-strength at life=0.5.
        CHECK(p_b.velocity.x() == doctest::Approx(p_a.velocity.x() * 0.5));
    }
}

// ===========================================================================
// velocityrandom inheritcontrolpointvelocity sub-flag
// ===========================================================================

TEST_SUITE("velocityrandom inherit-cp-velocity flag") {
    TEST_CASE("flag absent: only the random velocity is applied") {
        std::array<ParticleControlpoint, 8> cps;
        cps[0].velocity = Eigen::Vector3d(50, 0, 0);
        json j = { { "name", "velocityrandom" },
                   { "min", "0 0 0" }, { "max", "0 0 0" } };
        auto init = WPParticleParser::genParticleInitOp(
            j, std::span<const ParticleControlpoint>(cps.data(), cps.size()));
        Particle p;
        p.velocity = Eigen::Vector3f(0, 0, 0);
        init(p, 0.0);
        CHECK(p.velocity.x() == doctest::Approx(0.0));
    }

    TEST_CASE("flag set: CP velocity is added on top of the random velocity") {
        std::array<ParticleControlpoint, 8> cps;
        cps[0].velocity = Eigen::Vector3d(50, 0, 0);
        json j = { { "name", "velocityrandom" },
                   { "min", "0 0 0" }, { "max", "0 0 0" },
                   { "inheritcontrolpointvelocity", true },
                   { "controlpoint", 0 } };
        auto init = WPParticleParser::genParticleInitOp(
            j, std::span<const ParticleControlpoint>(cps.data(), cps.size()));
        Particle p;
        p.velocity = Eigen::Vector3f(0, 0, 0);
        init(p, 0.0);
        // min=max=1 → exactly the CP velocity is added.
        CHECK(p.velocity.x() == doctest::Approx(50.0));
    }

    TEST_CASE("flag set: CP velocity adds to a non-zero random base") {
        std::array<ParticleControlpoint, 8> cps;
        cps[0].velocity = Eigen::Vector3d(50, 0, 0);
        json j = { { "name", "velocityrandom" },
                   { "min", "10 0 0" }, { "max", "10 0 0" },
                   { "inheritcontrolpointvelocity", true },
                   { "controlpoint", 0 } };
        auto init = WPParticleParser::genParticleInitOp(
            j, std::span<const ParticleControlpoint>(cps.data(), cps.size()));
        Particle p;
        p.velocity = Eigen::Vector3f(0, 0, 0);
        init(p, 0.0);
        // 10 (random) + 50 (CP) = 60.
        CHECK(p.velocity.x() == doctest::Approx(60.0));
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
// inheritinitialvaluefromevent initializer
// ===========================================================================

namespace
{

// Builds an event-bound child instance whose parent has one live particle with
// the supplied state, sets the spawn-context thread-local, and returns both
// instances so the test can clear the context after running the initializer.
struct EventInheritFixture {
    ParticleInstance parent;
    ParticleInstance child;

    EventInheritFixture(const Particle& seed) {
        parent.ParticlesVec().push_back(seed);
        child.GetBoundedData().parent       = &parent;
        child.GetBoundedData().particle_idx = 0;
        particle_spawn_context::SetSpawnInstance(&child);
    }
    ~EventInheritFixture() {
        particle_spawn_context::SetSpawnInstance(nullptr);
    }
};

Particle makeParent() {
    Particle p;
    p.lifetime = 1.5f;
    p.alpha    = 0.6f;
    p.size     = 42.0f;
    p.color    = Eigen::Vector3f(0.2f, 0.4f, 0.8f);
    p.velocity = Eigen::Vector3f(7.0f, -3.0f, 11.0f);
    p.rotation = Eigen::Vector3f(0.1f, 0.2f, 0.3f);
    return p;
}

} // namespace

TEST_SUITE("inheritinitialvaluefromevent") {
    TEST_CASE("color: copies parent rgb into init+current") {
        EventInheritFixture fx(makeParent());
        json j = { { "name", "inheritinitialvaluefromevent" }, { "input", "color" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(0.2f));
        CHECK(p.color.y() == doctest::Approx(0.4f));
        CHECK(p.color.z() == doctest::Approx(0.8f));
        CHECK(p.init.color.x() == doctest::Approx(0.2f));
    }

    TEST_CASE("size and alpha: copies parent into init+current") {
        EventInheritFixture fx(makeParent());
        json j_size  = { { "name", "inheritinitialvaluefromevent" }, { "input", "size" } };
        json j_alpha = { { "name", "inheritinitialvaluefromevent" }, { "input", "alpha" } };
        auto i_size  = WPParticleParser::genParticleInitOp(j_size);
        auto i_alpha = WPParticleParser::genParticleInitOp(j_alpha);
        Particle p;
        i_size(p, 0.0);
        i_alpha(p, 0.0);
        CHECK(p.size == doctest::Approx(42.0f));
        CHECK(p.init.size == doctest::Approx(42.0f));
        CHECK(p.alpha == doctest::Approx(0.6f));
        CHECK(p.init.alpha == doctest::Approx(0.6f));
    }

    TEST_CASE("velocity and rotation: copies parent vec3") {
        EventInheritFixture fx(makeParent());
        json j_vel = { { "name", "inheritinitialvaluefromevent" }, { "input", "velocity" } };
        json j_rot = { { "name", "inheritinitialvaluefromevent" }, { "input", "rotation" } };
        auto i_vel = WPParticleParser::genParticleInitOp(j_vel);
        auto i_rot = WPParticleParser::genParticleInitOp(j_rot);
        Particle p;
        i_vel(p, 0.0);
        i_rot(p, 0.0);
        CHECK(p.velocity.x() == doctest::Approx(7.0f));
        CHECK(p.velocity.y() == doctest::Approx(-3.0f));
        CHECK(p.velocity.z() == doctest::Approx(11.0f));
        CHECK(p.rotation.x() == doctest::Approx(0.1f));
        CHECK(p.rotation.z() == doctest::Approx(0.3f));
    }

    TEST_CASE("lifetime: copies parent's remaining lifetime into init+current") {
        EventInheritFixture fx(makeParent());
        json j = { { "name", "inheritinitialvaluefromevent" }, { "input", "lifetime" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        p.lifetime      = 999.0f;
        p.init.lifetime = 999.0f;
        init(p, 0.0);
        CHECK(p.lifetime == doctest::Approx(1.5f));
        CHECK(p.init.lifetime == doctest::Approx(1.5f));
    }

    TEST_CASE("no spawn context → no-op (defensive: child stays at default)") {
        // Don't touch the thread-local — emulate "called outside an emit cycle".
        json j = { { "name", "inheritinitialvaluefromevent" }, { "input", "color" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        Eigen::Vector3f original = p.color;
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(original.x()));
    }

    TEST_CASE("dead parent particle → no-op (preserves prior initializer output)") {
        Particle dead = makeParent();
        dead.lifetime = 0.0f;
        EventInheritFixture fx(dead);
        json j = { { "name", "inheritinitialvaluefromevent" }, { "input", "size" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        p.size      = 7.0f;
        p.init.size = 7.0f;
        init(p, 0.0);
        CHECK(p.size == doctest::Approx(7.0f));
        CHECK(p.init.size == doctest::Approx(7.0f));
    }

    TEST_CASE("unknown input string → no-op (typo-safe)") {
        EventInheritFixture fx(makeParent());
        json j = { { "name", "inheritinitialvaluefromevent" }, { "input", "bogus" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        Eigen::Vector3f original_color = p.color;
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(original_color.x()));
    }

    TEST_CASE("aliases: opacity ≡ alpha, particlecolor ≡ color") {
        EventInheritFixture fx(makeParent());
        json j_op = { { "name", "inheritinitialvaluefromevent" }, { "input", "opacity" } };
        json j_pc = { { "name", "inheritinitialvaluefromevent" }, { "input", "particlecolor" } };
        auto i_op = WPParticleParser::genParticleInitOp(j_op);
        auto i_pc = WPParticleParser::genParticleInitOp(j_pc);
        Particle p;
        i_op(p, 0.0);
        i_pc(p, 0.0);
        CHECK(p.alpha == doctest::Approx(0.6f));
        CHECK(p.color.y() == doctest::Approx(0.4f));
    }
}

// ===========================================================================
// collisionmodel — stub
// ===========================================================================

TEST_SUITE("collisionmodel") {
    TEST_CASE("stub: parses without crash and runs as a no-op") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(10, 20, 30);
        p.velocity  = Eigen::Vector3f(1, 2, 3);
        json j      = { { "name", "collisionmodel" }, { "controlpoint", 0 } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.x() == doctest::Approx(10.0f));
        CHECK(p.velocity.z() == doctest::Approx(3.0f));
    }
}

// ===========================================================================
// collisionquad — finite quad with author basis
// ===========================================================================

TEST_SUITE("collisionquad") {
    TEST_CASE("particle crossing a horizontal quad gets snapped + reflected") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(0, -10, 0);  // crossed below the plane
        p.velocity  = Eigen::Vector3f(0, -50, 0);  // moving downward
        of.time_pass = 1.0;                         // 1s frame so prev_sd recovers as +40
        json j      = { { "name", "collisionquad" },
                        { "controlpoint", 0 },
                        { "plane", "0 1 0" },
                        { "forward", "1 0 0" },
                        { "size", "200 200" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.y() == doctest::Approx(0.0f));
        CHECK(p.velocity.y() == doctest::Approx(50.0f));
    }

    TEST_CASE("particle outside U/V bounds is unaffected") {
        OpFixture of;
        Particle& p = of.spawn();
        // Particle sits past the +X edge of a 100×100 quad.
        p.position  = Eigen::Vector3f(200, -10, 0);
        p.velocity  = Eigen::Vector3f(0, -50, 0);
        of.time_pass = 1.0;
        json j      = { { "name", "collisionquad" },
                        { "controlpoint", 0 },
                        { "plane", "0 1 0" },
                        { "forward", "1 0 0" },
                        { "size", "100 100" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.y() == doctest::Approx(-10.0f));  // unaffected
        CHECK(p.velocity.y() == doctest::Approx(-50.0f));
    }

    TEST_CASE("particle that did not cross within the frame is unaffected") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(0, 50, 0);  // above plane, moving up
        p.velocity  = Eigen::Vector3f(0, 10, 0);
        of.time_pass = 1.0;
        json j      = { { "name", "collisionquad" },
                        { "controlpoint", 0 },
                        { "plane", "0 1 0" },
                        { "forward", "1 0 0" },
                        { "size", "200 200" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.y() == doctest::Approx(50.0f));
        CHECK(p.velocity.y() == doctest::Approx(10.0f));
    }

    TEST_CASE("origin shifts quad centre off the CP anchor") {
        OpFixture of;
        of.cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        Particle& p = of.spawn();
        // Quad anchored at (10, 0, 0) via origin.  Particle crossed at +X centre.
        p.position  = Eigen::Vector3f(10, -5, 0);
        p.velocity  = Eigen::Vector3f(0, -10, 0);
        of.time_pass = 1.0;
        json j      = { { "name", "collisionquad" },
                        { "origin", "10 0 0" },
                        { "plane", "0 1 0" },
                        { "forward", "1 0 0" },
                        { "size", "20 20" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.y() == doctest::Approx(0.0f));
        CHECK(p.velocity.y() == doctest::Approx(10.0f));
    }

    TEST_CASE("forward parallel to plane normal: arbitrary perpendicular fallback") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(0, -5, 0);
        p.velocity  = Eigen::Vector3f(0, -50, 0);
        of.time_pass = 1.0;
        // Author bug: forward is colinear with plane normal — should still produce
        // a usable basis without dividing by zero.
        json j      = { { "name", "collisionquad" },
                        { "plane", "0 1 0" },
                        { "forward", "0 1 0" },
                        { "size", "200 200" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.y() == doctest::Approx(0.0f));
        CHECK(p.velocity.y() == doctest::Approx(50.0f));
    }
}

// ===========================================================================
// collisionbox / collisionbounds — AABB at CP
// ===========================================================================

TEST_SUITE("collisionbox") {
    TEST_CASE("particle inside the box is left alone") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(5, 5, 5);
        p.velocity  = Eigen::Vector3f(1, 0, 0);
        json j      = { { "name", "collisionbox" },
                        { "controlpoint", 0 },
                        { "halfsize", "100 100 100" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.x() == doctest::Approx(5.0f));
        CHECK(p.velocity.x() == doctest::Approx(1.0f));
    }

    TEST_CASE("particle past +X face: clamped to surface and X velocity flipped") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(150, 0, 0);
        p.velocity  = Eigen::Vector3f(20, 5, 0);
        json j      = { { "name", "collisionbox" },
                        { "controlpoint", 0 },
                        { "halfsize", "100 100 100" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.x() == doctest::Approx(100.0f));
        CHECK(p.velocity.x() == doctest::Approx(-20.0f));
        // Y velocity untouched — box is per-axis.
        CHECK(p.velocity.y() == doctest::Approx(5.0f));
    }

    TEST_CASE("particle past -Y face: clamped and Y velocity flipped") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(0, -200, 0);
        p.velocity  = Eigen::Vector3f(0, -10, 0);
        json j      = { { "name", "collisionbox" },
                        { "controlpoint", 0 },
                        { "halfsize", "100 100 100" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.y() == doctest::Approx(-100.0f));
        CHECK(p.velocity.y() == doctest::Approx(10.0f));
    }

    TEST_CASE("box anchored at non-zero CP") {
        OpFixture of;
        of.cps[2].resolved = Eigen::Vector3d(50, 50, 0);
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(170, 50, 0);  // +X past CP+halfsize
        p.velocity  = Eigen::Vector3f(5, 0, 0);
        json j      = { { "name", "collisionbox" },
                        { "controlpoint", 2 },
                        { "halfsize", "100 100 100" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.x() == doctest::Approx(150.0f));  // CP.x + halfsize.x
        CHECK(p.velocity.x() == doctest::Approx(-5.0f));
    }

    TEST_CASE("restitution scales the bounce") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(120, 0, 0);
        p.velocity  = Eigen::Vector3f(10, 0, 0);
        json j      = { { "name", "collisionbox" },
                        { "controlpoint", 0 },
                        { "halfsize", "100 100 100" },
                        { "restitution", 0.5f } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.velocity.x() == doctest::Approx(-5.0f));
    }

    TEST_CASE("velocity already moving away: position clamped but velocity preserved") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(120, 0, 0);
        p.velocity  = Eigen::Vector3f(-3, 0, 0);  // already moving inward
        json j      = { { "name", "collisionbox" },
                        { "controlpoint", 0 },
                        { "halfsize", "100 100 100" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.x() == doctest::Approx(100.0f));
        CHECK(p.velocity.x() == doctest::Approx(-3.0f));  // not flipped
    }

    TEST_CASE("collisionbounds shares the box implementation") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(150, 0, 0);
        p.velocity  = Eigen::Vector3f(20, 0, 0);
        json j      = { { "name", "collisionbounds" },
                        { "controlpoint", 0 },
                        { "halfsize", "100 100 100" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.x() == doctest::Approx(100.0f));
        CHECK(p.velocity.x() == doctest::Approx(-20.0f));
    }

    TEST_CASE("dead particle is skipped") {
        OpFixture of;
        Particle& p = of.spawn();
        p.position  = Eigen::Vector3f(150, 0, 0);
        p.velocity  = Eigen::Vector3f(5, 0, 0);
        p.lifetime  = 0.0f;
        json j      = { { "name", "collisionbox" }, { "halfsize", "100 100 100" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(of.info());
        CHECK(p.position.x() == doctest::Approx(150.0f));  // untouched
    }
}

// ===========================================================================
// inheritvaluefromevent operator — per-frame parent property tracking
// ===========================================================================

namespace
{

struct InheritOpFixture {
    ParticleInstance parent;
    ParticleInstance child;
    OpFixture        of;

    explicit InheritOpFixture(const Particle& seed) {
        parent.ParticlesVec().push_back(seed);
        child.GetBoundedData().parent       = &parent;
        child.GetBoundedData().particle_idx = 0;
    }

    ParticleInfo info() {
        ParticleInfo i = of.info();
        i.instance     = &child;
        return i;
    }
};

} // namespace

TEST_SUITE("inheritvaluefromevent") {
    TEST_CASE("color: fully overwrites child each tick (no blend)") {
        InheritOpFixture fx(makeParent());
        Particle& p = fx.of.spawn();
        p.color     = Eigen::Vector3f(0, 0, 0);
        json j      = { { "name", "inheritvaluefromevent" }, { "input", "color" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(fx.info());
        CHECK(p.color.x() == doctest::Approx(0.2f));
        CHECK(p.color.y() == doctest::Approx(0.4f));
        CHECK(p.color.z() == doctest::Approx(0.8f));
    }

    TEST_CASE("size and alpha overwrite each tick") {
        InheritOpFixture fx(makeParent());
        Particle& p = fx.of.spawn();
        p.size      = 5.0f;
        p.alpha     = 0.0f;
        json j_size = { { "name", "inheritvaluefromevent" }, { "input", "size" } };
        json j_a    = { { "name", "inheritvaluefromevent" }, { "input", "alpha" } };
        auto op_s   = WPParticleParser::genParticleOperatorOp(j_size, empty_override());
        auto op_a   = WPParticleParser::genParticleOperatorOp(j_a, empty_override());
        op_s(fx.info());
        op_a(fx.info());
        CHECK(p.size == doctest::Approx(42.0f));
        CHECK(p.alpha == doctest::Approx(0.6f));
    }

    TEST_CASE("velocity overwrite tracks parent velocity") {
        InheritOpFixture fx(makeParent());
        Particle& p = fx.of.spawn();
        p.velocity  = Eigen::Vector3f(0, 0, 0);
        json j      = { { "name", "inheritvaluefromevent" }, { "input", "velocity" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(fx.info());
        CHECK(p.velocity.x() == doctest::Approx(7.0f));
        CHECK(p.velocity.z() == doctest::Approx(11.0f));
    }

    TEST_CASE("blend-window halves the override") {
        InheritOpFixture fx(makeParent());
        Particle& p = fx.of.spawn();
        p.size      = 0.0f;
        // Particle at LifetimePos = 0.5; blend ramps in over [0, 0.5] and out over [0.5, 1].
        p.lifetime      = 0.5f;
        p.init.lifetime = 1.0f;
        // Blend in from 0..0.5 (factor at life=0.5 is 1) — but we want a partial factor
        // so authour fade-in extending past current life: 0..1.0 makes factor=0.5.
        json j = { { "name", "inheritvaluefromevent" },
                   { "input", "size" },
                   { "blendinstart", 0.0 },
                   { "blendinend", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(fx.info());
        // factor = 0.5 → size = 0*(0.5) + 42*(0.5) = 21
        CHECK(p.size == doctest::Approx(21.0f));
    }

    TEST_CASE("dead parent: child is untouched (no-op guard)") {
        Particle dead = makeParent();
        dead.lifetime = 0.0f;
        InheritOpFixture fx(dead);
        Particle& p = fx.of.spawn();
        p.size      = 7.0f;
        json j      = { { "name", "inheritvaluefromevent" }, { "input", "size" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(fx.info());
        CHECK(p.size == doctest::Approx(7.0f));
    }

    TEST_CASE("null instance: no-op (defensive guard)") {
        OpFixture of;
        Particle& p = of.spawn();
        p.size      = 99.0f;
        json j      = { { "name", "inheritvaluefromevent" }, { "input", "size" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        // info.instance is nullptr by default.
        op(of.info());
        CHECK(p.size == doctest::Approx(99.0f));
    }

    TEST_CASE("dead child particle: skipped by lifetime gate") {
        InheritOpFixture fx(makeParent());
        Particle& p = fx.of.spawn();
        p.size      = 3.0f;
        p.lifetime  = 0.0f;
        json j      = { { "name", "inheritvaluefromevent" }, { "input", "size" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(fx.info());
        CHECK(p.size == doctest::Approx(3.0f));
    }

    TEST_CASE("unknown input string: silent no-op") {
        InheritOpFixture fx(makeParent());
        Particle& p = fx.of.spawn();
        p.size      = 11.0f;
        json j      = { { "name", "inheritvaluefromevent" }, { "input", "noeffect" } };
        auto op     = WPParticleParser::genParticleOperatorOp(j, empty_override());
        op(fx.info());
        CHECK(p.size == doctest::Approx(11.0f));
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
