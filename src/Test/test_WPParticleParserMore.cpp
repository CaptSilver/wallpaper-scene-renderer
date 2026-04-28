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
#include <vector>

using namespace wallpaper;
using nlohmann::json;

namespace
{

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
// Initializers
// ===========================================================================

TEST_SUITE("lifetimerandom initializer") {
    TEST_CASE("min == max gives a near-constant lifetime (within ±5% jitter)") {
        json j = { { "name", "lifetimerandom" }, { "min", 2.0 }, { "max", 2.0 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 10; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(p.lifetime >= 1.9f);
            CHECK(p.lifetime <= 2.1f);
        }
    }

    TEST_CASE("range produces values inside [min, max]") {
        json j = { { "name", "lifetimerandom" }, { "min", 0.5 }, { "max", 3.0 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 30; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(p.lifetime >= 0.5f);
            CHECK(p.lifetime <= 3.0f);
        }
    }
}

TEST_SUITE("sizerandom initializer") {
    TEST_CASE("range bounds are honoured") {
        json j = { { "name", "sizerandom" }, { "min", 5.0 }, { "max", 50.0 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 30; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(p.size >= 5.0f);
            CHECK(p.size <= 50.0f);
        }
    }

    TEST_CASE("min == max gives an exact size") {
        json j = { { "name", "sizerandom" }, { "min", 12.5 }, { "max", 12.5 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        init(p, 0.0);
        CHECK(p.size == doctest::Approx(12.5f));
    }
}

TEST_SUITE("alpharandom initializer") {
    TEST_CASE("range bounds are honoured") {
        json j = { { "name", "alpharandom" }, { "min", 0.2 }, { "max", 0.8 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 30; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(p.alpha >= 0.2f);
            CHECK(p.alpha <= 0.8f);
        }
    }
}

TEST_SUITE("velocityrandom initializer") {
    TEST_CASE("velocity components fall within configured range") {
        json j = { { "name", "velocityrandom" },
                   { "min", "-10 -20 -30" }, { "max", "10 20 30" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 30; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(std::abs(p.velocity.x()) <= 10.0f);
            CHECK(std::abs(p.velocity.y()) <= 20.0f);
            CHECK(std::abs(p.velocity.z()) <= 30.0f);
        }
    }
}

TEST_SUITE("rotationrandom initializer") {
    TEST_CASE("rotation z within [0, 2π] by default") {
        json j    = { { "name", "rotationrandom" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 20; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(p.rotation.z() >= 0.0f);
            CHECK(p.rotation.z() <= 2.0f * (float)M_PI);
        }
    }
}

TEST_SUITE("box initializer") {
    TEST_CASE("particle position falls within configured box") {
        json j    = { { "name", "box" }, { "min", "-5 -5 0" }, { "max", "5 5 0" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 30; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(std::abs(p.position.x()) <= 5.0f);
            CHECK(std::abs(p.position.y()) <= 5.0f);
            CHECK(p.position.z() == doctest::Approx(0.0f));
        }
    }
}

TEST_SUITE("sphere initializer") {
    TEST_CASE("particle position lies within outer radius") {
        json j    = { { "name", "sphere" }, { "min", 0.0 }, { "max", 10.0 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 30; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(p.position.norm() <= 10.0f);
        }
    }
}

TEST_SUITE("colorrandom initializer") {
    TEST_CASE("RGB components in [0, 1] when range is full") {
        json j    = { { "name", "colorrandom" },
                      { "min", "0 0 0" }, { "max", "255 255 255" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 30; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(p.color.x() >= 0.0f);
            CHECK(p.color.x() <= 1.0f);
            CHECK(p.color.y() >= 0.0f);
            CHECK(p.color.y() <= 1.0f);
            CHECK(p.color.z() >= 0.0f);
            CHECK(p.color.z() <= 1.0f);
        }
    }

    TEST_CASE("min only with absent max → max defaults to min") {
        json j    = { { "name", "colorrandom" }, { "min", "127 0 0" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(127.0f / 255.0f));
        CHECK(p.color.y() == doctest::Approx(0.0f));
        CHECK(p.color.z() == doctest::Approx(0.0f));
    }
}

// ===========================================================================
// Operators
// ===========================================================================

TEST_SUITE("movement operator") {
    TEST_CASE("constant velocity advances position") {
        json j = { { "name", "movement" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.velocity  = Eigen::Vector3f(10, 0, 0);
        fx.time_pass = 1.0;
        op(fx.info());
        CHECK(p.position.x() == doctest::Approx(10.0));
    }

    TEST_CASE("gravity accelerates particle each tick") {
        json j   = { { "name", "movement" }, { "gravity", "0 -9.8 0" } };
        auto op  = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p  = fx.spawn();
        fx.time_pass = 1.0;
        op(fx.info());
        // With gravity (0, -9.8, 0), spd default 1, factor 1, dt 1 — velocity gains -9.8 in y
        CHECK(p.velocity.y() == doctest::Approx(-9.8f));
    }

    TEST_CASE("drag attenuates velocity") {
        json j   = { { "name", "movement" }, { "drag", 0.5 } };
        auto op  = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p  = fx.spawn();
        p.velocity   = Eigen::Vector3f(100, 0, 0);
        fx.time_pass = 1.0;
        op(fx.info());
        // factor = max(0, 1 - 0.5*1.0*1.0) = 0.5 → velocity halved
        CHECK(p.velocity.x() == doctest::Approx(50.0f));
    }
}

TEST_SUITE("alphafade operator") {
    TEST_CASE("fades alpha to zero in the last fadeouttime seconds") {
        json j   = { { "name", "alphafade" }, { "fadeintime", 0.0 }, { "fadeouttime", 0.5 } };
        auto op  = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p     = fx.spawn();
        p.init.lifetime = 1.0f; // 1s life, fade-out begins at life=0.5
        p.lifetime      = 0.5f; // 50% of life remaining → at exact start of fade-out window
        op(fx.info());
        // life pos = (1 - 0.5/1) = 0.5. out_start = 1 - 0.5 = 0.5. life > out_start? life=0.5 → no.
        // alpha unchanged (= 1.0)
        CHECK(p.alpha == doctest::Approx(1.0f));
    }

    TEST_CASE("fade-out kicks in once lifetime crosses out_start") {
        json j   = { { "name", "alphafade" }, { "fadeintime", 0.0 }, { "fadeouttime", 0.5 } };
        auto op  = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p     = fx.spawn();
        p.init.lifetime = 1.0f;
        p.lifetime      = 0.25f; // life pos = 0.75 → 50% into 0.5..1.0 fade-out window
        op(fx.info());
        // a *= 1 - (0.75 - 0.5) / 0.5 = 1 - 0.5 = 0.5
        CHECK(p.alpha == doctest::Approx(0.5f));
    }

    TEST_CASE("fade-in ramps alpha from 0 across fadeintime seconds") {
        json j  = { { "name", "alphafade" }, { "fadeintime", 0.5 }, { "fadeouttime", 0.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p     = fx.spawn();
        p.init.lifetime = 1.0f;
        p.lifetime      = 0.75f; // life pos = 0.25 → 50% into 0..0.5 fade-in
        op(fx.info());
        // a *= 0.25 / 0.5 = 0.5
        CHECK(p.alpha == doctest::Approx(0.5f));
    }
}

TEST_SUITE("alphachange operator") {
    TEST_CASE("interpolates alpha between startvalue and endvalue across [start, end] life") {
        json j = { { "name", "alphachange" },
                   { "starttime", 0.0 }, { "endtime", 1.0 },
                   { "startvalue", 1.0 }, { "endvalue", 0.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        // At lifetime pos = 0.5 (mid-life): expected fade = 0.5
        p.lifetime = 0.5f;
        p.alpha    = 1.0f;
        op(fx.info());
        CHECK(p.alpha == doctest::Approx(0.5f));
    }
}

TEST_SUITE("colorchange operator") {
    TEST_CASE("interpolates RGB triplet between start/end values") {
        json j = { { "name", "colorchange" },
                   { "starttime", 0.0 }, { "endtime", 1.0 },
                   { "startvalue", "1.0 0.0 0.0" }, { "endvalue", "0.0 0.0 1.0" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.lifetime  = 0.5f; // mid-life
        p.color     = Eigen::Vector3f(1, 1, 1);
        op(fx.info());
        // multiplier = (0.5, 0, 0.5); applied multiplicatively
        CHECK(p.color.x() == doctest::Approx(0.5f));
        CHECK(p.color.y() == doctest::Approx(0.0f));
        CHECK(p.color.z() == doctest::Approx(0.5f));
    }
}

TEST_SUITE("sizechange operator") {
    TEST_CASE("interpolates size multiplier between start/end values") {
        json j = { { "name", "sizechange" },
                   { "starttime", 0.0 }, { "endtime", 1.0 },
                   { "startvalue", 1.0 }, { "endvalue", 4.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.lifetime  = 0.5f; // mid-life: factor = 2.5
        p.size      = 10.0f;
        op(fx.info());
        CHECK(p.size == doctest::Approx(25.0f));
    }
}

TEST_SUITE("controlpointattract operator") {
    TEST_CASE("pulls particle toward CP within threshold") {
        json j = { { "name", "controlpointattract" },
                   { "controlpoint", 0 }, { "scale", 100.0 },
                   { "threshold", 50.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(10, 0, 0); // 10 units from origin (CP0 at 0,0,0)
        // After op, velocity should accelerate toward origin: -x direction
        op(fx.info());
        CHECK(p.velocity.x() < 0.0f);
    }

    TEST_CASE("ignores particles beyond threshold") {
        json j = { { "name", "controlpointattract" },
                   { "controlpoint", 0 }, { "scale", 100.0 }, { "threshold", 5.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(10, 0, 0); // outside threshold
        op(fx.info());
        CHECK(p.velocity.x() == doctest::Approx(0.0f));
    }
}

TEST_SUITE("controlpointforce operator (repel sibling)") {
    TEST_CASE("pushes particle away from CP within threshold") {
        json j = { { "name", "controlpointforce" },
                   { "controlpoint", 0 }, { "scale", 100.0 }, { "threshold", 50.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(10, 0, 0);
        op(fx.info());
        // Repulsion pushes +x
        CHECK(p.velocity.x() > 0.0f);
    }
}

TEST_SUITE("oscillatealpha operator") {
    TEST_CASE("oscillation drives alpha away from 1.0 over time") {
        json j = { { "name", "oscillatealpha" },
                   { "frequencymin", 1.0 }, { "frequencymax", 1.0 },
                   { "min", 0.0 }, { "max", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        for (int i = 0; i < 5; i++) fx.spawn();
        // Drive several ticks; track alpha over time.
        for (int t = 0; t < 20; t++) {
            for (auto& p : fx.particles) p.alpha = 1.0f;
            op(fx.info());
        }
        // After many oscillations, expect at least one alpha to be < 1
        bool any_modulated = false;
        for (const auto& p : fx.particles) {
            if (p.alpha < 0.99f) { any_modulated = true; break; }
        }
        CHECK(any_modulated);
    }
}

TEST_SUITE("oscillatesize operator") {
    TEST_CASE("modulates size from baseline over time") {
        json j = { { "name", "oscillatesize" },
                   { "frequencymin", 1.0 }, { "frequencymax", 1.0 },
                   { "min", 0.5 }, { "max", 2.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        for (int i = 0; i < 5; i++) fx.spawn();
        for (int t = 0; t < 20; t++) {
            for (auto& p : fx.particles) p.size = 20.0f;
            op(fx.info());
        }
        bool any_modulated = false;
        for (const auto& p : fx.particles) {
            if (std::abs(p.size - 20.0f) > 0.1f) { any_modulated = true; break; }
        }
        CHECK(any_modulated);
    }
}

TEST_SUITE("inheritcontrolpointvelocity initializer") {
    TEST_CASE("copies CP velocity into newly-spawned particle") {
        std::array<ParticleControlpoint, 8> cps;
        cps[2].velocity = Eigen::Vector3d(7, 8, 9);
        // Lock the scale factor to 1 so the expected output is deterministic;
        // otherwise it's randomised in [0, 1].
        json j = { { "name", "inheritcontrolpointvelocity" },
                   { "controlpoint", 2 }, { "min", 1.0 }, { "max", 1.0 } };
        auto init = WPParticleParser::genParticleInitOp(j, std::span<const ParticleControlpoint>(cps.data(), cps.size()));
        Particle p;
        init(p, 0.0);
        CHECK(p.velocity.x() == doctest::Approx(7.0));
        CHECK(p.velocity.y() == doctest::Approx(8.0));
        CHECK(p.velocity.z() == doctest::Approx(9.0));
    }

    TEST_CASE("clamped CP index applies (controlpoint > 7 → 7)") {
        std::array<ParticleControlpoint, 8> cps;
        cps[7].velocity = Eigen::Vector3d(1, 0, 0);
        json j    = { { "name", "inheritcontrolpointvelocity" }, { "controlpoint", 99 },
                      { "min", 1.0 }, { "max", 1.0 } };
        auto init = WPParticleParser::genParticleInitOp(j, std::span<const ParticleControlpoint>(cps.data(), cps.size()));
        Particle p;
        init(p, 0.0);
        CHECK(p.velocity.x() == doctest::Approx(1.0));
    }
}

TEST_SUITE("collisionplane operator") {
    TEST_CASE("particle moving into the plane is reflected") {
        json j = { { "name", "collisionplane" },
                   { "planenormal", "0 1 0" },
                   { "planedistance", 0.0 },
                   { "elasticity", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(0, -1, 0); // below plane
        p.velocity  = Eigen::Vector3f(0, -2, 0); // moving down further
        op(fx.info());
        // After collision, vertical velocity should flip sign (or be zero/positive)
        CHECK(p.velocity.y() >= 0.0f);
    }
}

TEST_SUITE("collisionsphere operator") {
    TEST_CASE("particle inside the sphere bounces outward") {
        json j = { { "name", "collisionsphere" },
                   { "controlpoint", 0 },
                   { "radius", 5.0 }, { "elasticity", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(2, 0, 0); // inside sphere of radius 5 at origin
        p.velocity  = Eigen::Vector3f(0, 0, 0);
        op(fx.info());
        // Position should have been pushed to the surface (distance ~5)
        CHECK(p.position.norm() >= 4.5f);
    }
}

TEST_SUITE("inheritvaluefromevent operator") {
    TEST_CASE("no-op when ParticleInfo.instance is null") {
        json j  = { { "name", "inheritvaluefromevent" }, { "input", "color" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.color     = Eigen::Vector3f(0.4f, 0.5f, 0.6f);
        op(fx.info()); // info.instance is null (default fixture)
        // unchanged
        CHECK(p.color.x() == doctest::Approx(0.4f));
        CHECK(p.color.y() == doctest::Approx(0.5f));
        CHECK(p.color.z() == doctest::Approx(0.6f));
    }
}

TEST_SUITE("vortex operator") {
    TEST_CASE("inside-inner zone applies speedinner force tangential to axis") {
        json j = { { "name", "vortex" },
                   { "controlpoint",   0 },
                   { "axis",           "0 0 1" },
                   { "speedinner",     100.0 },
                   { "speedouter",     0.0 },
                   { "distanceinner",  100.0 },
                   { "distanceouter",  200.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(50, 0, 0); // distance 50, well inside inner
        op(fx.info());
        // Tangential to z axis at (50, 0, 0) means velocity gains +y (or -y) component.
        CHECK(std::abs(p.velocity.y()) > 0.0f);
    }

    TEST_CASE("particles outside outer zone receive speedouter force") {
        json j = { { "name", "vortex" },
                   { "controlpoint",  0 },
                   { "axis",          "0 0 1" },
                   { "speedinner",    0.0 },
                   { "speedouter",    100.0 },
                   { "distanceinner", 10.0 },
                   { "distanceouter", 50.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(100, 0, 0); // distance 100 > outer 50
        op(fx.info());
        CHECK(std::abs(p.velocity.y()) > 0.0f);
    }
}

TEST_SUITE("maintaindistancebetweencontrolpoints operator") {
    TEST_CASE("particle past CP1 endpoint is clamped onto the line") {
        json j  = { { "name", "maintaindistancebetweencontrolpoints" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        fx.cps[1].resolved = Eigen::Vector3d(10, 0, 0);
        Particle& p       = fx.spawn();
        p.position        = Eigen::Vector3f(20, 5, 0); // past CP1 along x
        op(fx.info());
        // After clamp: along-line component pulled back to 10; perpendicular y=5 stays.
        CHECK(p.position.x() == doctest::Approx(10.0f));
        CHECK(p.position.y() == doctest::Approx(5.0f));
    }

    TEST_CASE("particle within line range is left alone") {
        json j  = { { "name", "maintaindistancebetweencontrolpoints" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        fx.cps[1].resolved = Eigen::Vector3d(10, 0, 0);
        Particle& p       = fx.spawn();
        p.position        = Eigen::Vector3f(5, 3, 0); // mid-line
        op(fx.info());
        CHECK(p.position.x() == doctest::Approx(5.0f));
        CHECK(p.position.y() == doctest::Approx(3.0f));
    }

    TEST_CASE("particle past CP0 endpoint is clamped to origin") {
        json j  = { { "name", "maintaindistancebetweencontrolpoints" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        fx.cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        fx.cps[1].resolved = Eigen::Vector3d(10, 0, 0);
        Particle& p       = fx.spawn();
        p.position        = Eigen::Vector3f(-5, 2, 0);
        op(fx.info());
        CHECK(p.position.x() == doctest::Approx(0.0f));
        CHECK(p.position.y() == doctest::Approx(2.0f));
    }
}

TEST_SUITE("reducemovementnearcontrolpoint operator") {
    TEST_CASE("near a CP with reduction=1 attenuates velocity") {
        json j = { { "name", "reducemovementnearcontrolpoint" },
                   { "controlpoint",   0 },
                   { "distanceinner",  0.0 },
                   { "distanceouter",  10.0 },
                   { "reductioninner", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.velocity  = Eigen::Vector3f(10, 0, 0);
        p.position  = Eigen::Vector3f(0, 0, 0); // exactly at CP
        fx.time_pass = 1.0;
        op(fx.info());
        // factor = 1 / (1 + 100*1) = 1/101 ≈ 0.0099 → velocity ≈ 0.099
        CHECK(p.velocity.x() < 1.0f);
    }

    TEST_CASE("particle outside outer threshold left untouched") {
        json j = { { "name", "reducemovementnearcontrolpoint" },
                   { "controlpoint",   0 },
                   { "distanceinner",  0.0 },
                   { "distanceouter",  10.0 },
                   { "reductioninner", 100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(20, 0, 0); // > outer
        p.velocity  = Eigen::Vector3f(5, 0, 0);
        op(fx.info());
        CHECK(p.velocity.x() == doctest::Approx(5.0f));
    }
}

TEST_SUITE("positionoffsetrandom initializer") {
    TEST_CASE("zero distance is a no-op") {
        json j    = { { "name", "positionoffsetrandom" }, { "distance", 0.0 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        Eigen::Vector3f start = p.position;
        init(p, 0.0);
        CHECK(p.position.isApprox(start));
    }

    TEST_CASE("non-zero distance offsets the particle by < distance*0.02") {
        json j    = { { "name", "positionoffsetrandom" }, { "distance", 100.0 } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 20; i++) {
            Particle p;
            init(p, 0.0);
            // Spec says r = distance * 0.02 * cbrt(rand[0,1)), so |offset| <= 100*0.02 = 2
            CHECK(p.position.norm() <= 2.05f);
        }
    }
}

TEST_SUITE("rotationrandom initializer — explicit min/max") {
    TEST_CASE("explicit per-component bounds honored") {
        json j = { { "name", "rotationrandom" },
                   { "min", "0 0 1" }, { "max", "0 0 1" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        Particle p;
        init(p, 0.0);
        CHECK(p.rotation.x() == doctest::Approx(0.0f));
        CHECK(p.rotation.y() == doctest::Approx(0.0f));
        CHECK(p.rotation.z() == doctest::Approx(1.0f));
    }
}

TEST_SUITE("angularvelocityrandom initializer") {
    TEST_CASE("explicit min/max bounds honored") {
        json j = { { "name", "angularvelocityrandom" },
                   { "min", "0 0 -3" }, { "max", "0 0 3" } };
        auto init = WPParticleParser::genParticleInitOp(j);
        for (int i = 0; i < 30; i++) {
            Particle p;
            init(p, 0.0);
            CHECK(p.angularVelocity.x() == doctest::Approx(0.0f));
            CHECK(p.angularVelocity.y() == doctest::Approx(0.0f));
            CHECK(std::abs(p.angularVelocity.z()) <= 3.0f);
        }
    }
}

TEST_SUITE("angularmovement operator") {
    TEST_CASE("constant angular velocity advances rotation") {
        json j  = { { "name", "angularmovement" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p     = fx.spawn();
        p.angularVelocity = Eigen::Vector3f(0, 0, 1.0f);
        fx.time_pass    = 1.0;
        op(fx.info());
        // Rotation should advance in z (angularVelocity * time_pass)
        CHECK(p.rotation.z() != doctest::Approx(0.0f));
    }

    TEST_CASE("force accelerates angular velocity") {
        json j   = { { "name", "angularmovement" }, { "force", "0 0 5" } };
        auto op  = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p  = fx.spawn();
        fx.time_pass = 1.0;
        op(fx.info());
        // Positive force component imparts angular velocity in z.
        CHECK(p.angularVelocity.z() == doctest::Approx(5.0f));
    }
}

TEST_SUITE("colorchange operator with non-default times") {
    TEST_CASE("before starttime: holds startvalue (factor=0)") {
        json j = { { "name", "colorchange" },
                   { "starttime", 0.4 }, { "endtime", 0.8 },
                   { "startvalue", "0.5 0.5 0.5" }, { "endvalue", "1.0 1.0 1.0" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.lifetime  = 0.9f; // life-pos = 0.1, before starttime 0.4
        p.color     = Eigen::Vector3f(1, 1, 1);
        op(fx.info());
        // Below starttime → multiplier should be startvalue = 0.5
        CHECK(p.color.x() == doctest::Approx(0.5f));
    }
}

TEST_SUITE("genOverrideInitOp") {
    TEST_CASE("disabled override returns a no-op functor") {
        wpscene::ParticleInstanceoverride o = empty_override();
        auto init = WPParticleParser::genOverrideInitOp(o);
        Particle p;
        p.size = 30.0f;
        init(p, 0.0);
        CHECK(p.size == doctest::Approx(30.0f));
    }

    TEST_CASE("enabled override scales size + alpha multiplicatively") {
        wpscene::ParticleInstanceoverride o;
        o.enabled    = true;
        o.size       = 2.0f;
        o.alpha      = 0.5f;
        auto init    = WPParticleParser::genOverrideInitOp(o);
        Particle p;
        p.size       = 10.0f;
        p.alpha      = 1.0f;
        p.init.size  = 10.0f;
        p.init.alpha = 1.0f;
        init(p, 0.0);
        // Size doubles, alpha halves
        CHECK(p.size == doctest::Approx(20.0f));
        CHECK(p.alpha == doctest::Approx(0.5f));
    }

    TEST_CASE("color override replaces color when overColor is true") {
        // `over.color` is in 0..255 space (legacy WE convention); the override
        // path divides by 255 internally.  Pick values that map to clean 0..1
        // outputs.
        wpscene::ParticleInstanceoverride o;
        o.enabled   = true;
        o.overColor = true;
        o.color     = { 51.0f, 102.0f, 204.0f }; // → 0.2, 0.4, 0.8
        auto init   = WPParticleParser::genOverrideInitOp(o);
        Particle p;
        p.color     = Eigen::Vector3f(1, 1, 1);
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(0.2f));
        CHECK(p.color.y() == doctest::Approx(0.4f));
        CHECK(p.color.z() == doctest::Approx(0.8f));
    }

    TEST_CASE("colorn override (0..1 space) replaces color when overColorn is true") {
        wpscene::ParticleInstanceoverride o;
        o.enabled    = true;
        o.overColorn = true;
        o.colorn     = { 0.25f, 0.5f, 0.75f };
        auto init    = WPParticleParser::genOverrideInitOp(o);
        Particle p;
        p.color      = Eigen::Vector3f(1, 1, 1);
        init(p, 0.0);
        CHECK(p.color.x() == doctest::Approx(0.25f));
        CHECK(p.color.y() == doctest::Approx(0.5f));
        CHECK(p.color.z() == doctest::Approx(0.75f));
    }

    TEST_CASE("brightness > 1 multiplies color and reduces alpha by sqrt(brightness)") {
        wpscene::ParticleInstanceoverride o;
        o.enabled    = true;
        o.brightness = 4.0f;
        auto init    = WPParticleParser::genOverrideInitOp(o);
        Particle p;
        p.color      = Eigen::Vector3f(0.25f, 0.25f, 0.25f);
        p.alpha      = 1.0f;
        init(p, 0.0);
        // color *= 4 → 1.0
        CHECK(p.color.x() == doctest::Approx(1.0f));
        // alpha *= 1/sqrt(4) = 0.5
        CHECK(p.alpha == doctest::Approx(0.5f));
    }
}

TEST_SUITE("vortex_v2 operator") {
    TEST_CASE("inside-inner zone applies speedinner force") {
        json j = { { "name", "vortex_v2" },
                   { "controlpoint",  0 },
                   { "axis",          "0 0 1" },
                   { "speedinner",    100.0 },
                   { "speedouter",    0.0 },
                   { "distanceinner", 100.0 },
                   { "distanceouter", 200.0 },
                   { "ringradius",    0.0 },
                   { "ringwidth",     0.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(50, 0, 0);
        op(fx.info());
        // Tangential to z axis at (50, 0, 0): velocity gains ±y.
        CHECK(std::abs(p.velocity.y()) > 0.0f);
    }

    TEST_CASE("rotation flag rotates particle around the axis without imparting velocity") {
        json j = { { "name", "vortex_v2" },
                   { "controlpoint",  0 },
                   { "axis",          "0 0 1" },
                   { "speedinner",    1.0 },
                   { "speedouter",    1.0 },
                   { "distanceinner", 0.0 },
                   { "distanceouter", 1000.0 },
                   { "flags",         2 } }; // bit 1 = use_rotation
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(10, 0, 0);
        p.velocity  = Eigen::Vector3f(7, 7, 7);
        fx.time_pass = 0.5;
        op(fx.info());
        // Rotation mode zeros velocity and rotates position.
        CHECK(p.velocity.x() == doctest::Approx(0.0f));
        CHECK(p.velocity.y() == doctest::Approx(0.0f));
        CHECK(p.velocity.z() == doctest::Approx(0.0f));
        // Position should remain at distance ~10 from origin (rotation preserves radius).
        CHECK(p.position.norm() == doctest::Approx(10.0f));
    }

    TEST_CASE("ringradius pulls particle toward target ring") {
        json j = { { "name", "vortex_v2" },
                   { "controlpoint",      0 },
                   { "axis",              "0 0 1" },
                   { "speedinner",        0.0 },
                   { "speedouter",        0.0 },
                   { "distanceinner",     100.0 },
                   { "distanceouter",     200.0 },
                   { "ringradius",        50.0 },
                   { "ringwidth",         5.0 },
                   { "ringpulldistance",  100.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(100, 0, 0); // outside ring
        op(fx.info());
        // Pull is inward → -x velocity.
        CHECK(p.velocity.x() < 0.0f);
    }
}

TEST_SUITE("collisionbox operator") {
    TEST_CASE("particle past +x face is clamped and velocity reflected") {
        json j = { { "name", "collisionbox" },
                   { "controlpoint", 0 },
                   { "halfsize", "10 10 10" },
                   { "restitution", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(15, 0, 0); // 5 units past +x face
        p.velocity  = Eigen::Vector3f(20, 0, 0); // moving outward
        op(fx.info());
        CHECK(p.position.x() == doctest::Approx(10.0f));
        CHECK(p.velocity.x() == doctest::Approx(-20.0f));
    }

    TEST_CASE("particle past -y face is clamped and velocity reflected") {
        json j = { { "name", "collisionbox" },
                   { "controlpoint", 0 },
                   { "halfsize", "10 10 10" },
                   { "restitution", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(0, -15, 0);
        p.velocity  = Eigen::Vector3f(0, -8, 0);
        op(fx.info());
        CHECK(p.position.y() == doctest::Approx(-10.0f));
        CHECK(p.velocity.y() == doctest::Approx(8.0f));
    }

    TEST_CASE("particle inside box left untouched") {
        json j = { { "name", "collisionbox" },
                   { "controlpoint", 0 },
                   { "halfsize", "10 10 10" } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(3, -2, 1);
        p.velocity  = Eigen::Vector3f(5, 5, 5);
        op(fx.info());
        CHECK(p.position.x() == doctest::Approx(3.0f));
        CHECK(p.position.y() == doctest::Approx(-2.0f));
        CHECK(p.velocity.x() == doctest::Approx(5.0f));
    }
}

TEST_SUITE("collisionbounds alias of collisionbox") {
    TEST_CASE("collisionbounds dispatches the same per-axis logic as collisionbox") {
        json j = { { "name", "collisionbounds" },
                   { "controlpoint", 0 },
                   { "halfsize", "5 5 5" },
                   { "restitution", 1.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.position  = Eigen::Vector3f(8, 0, 0);
        p.velocity  = Eigen::Vector3f(3, 0, 0);
        op(fx.info());
        CHECK(p.position.x() == doctest::Approx(5.0f));
        CHECK(p.velocity.x() == doctest::Approx(-3.0f));
    }
}

TEST_SUITE("boids operator") {
    TEST_CASE("respects maxspeed clamp on a fast particle") {
        json j = { { "name", "boids" },
                   { "neighborthreshold",   0.0 },
                   { "separationthreshold", 0.0 },
                   { "separationfactor",    0.0 },
                   { "cohesionfactor",      0.0 },
                   { "alignmentfactor",     0.0 },
                   { "maxspeed",            10.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.velocity  = Eigen::Vector3f(50, 0, 0);
        op(fx.info());
        CHECK(p.velocity.norm() <= 10.0f + 1e-3f);
    }

    TEST_CASE("dead particles are skipped (no exception, no movement)") {
        json j  = { { "name", "boids" }, { "maxspeed", 50.0 } };
        auto op = WPParticleParser::genParticleOperatorOp(j, empty_override());
        OpFixture fx;
        Particle& p = fx.spawn();
        p.lifetime  = 0.0f;
        p.velocity  = Eigen::Vector3f(100, 0, 0);
        op(fx.info());
        // Dead particle's velocity not capped — it's skipped.
        CHECK(p.velocity.x() == doctest::Approx(100.0f));
    }
}

TEST_SUITE("mapsequencearoundcontrolpoint initializer") {
    TEST_CASE("places successive particles on a circle around the CP") {
        std::array<ParticleControlpoint, 8> cps;
        cps[0].resolved = Eigen::Vector3d(0, 0, 0);
        json j = { { "name", "mapsequencearoundcontrolpoint" },
                   { "count", 4 }, { "controlpoint", 0 }, { "axis", "10 10 0" } };
        auto init = WPParticleParser::genParticleInitOp(j, std::span<const ParticleControlpoint>(cps.data(), cps.size()));
        std::vector<Eigen::Vector3f> positions;
        for (int i = 0; i < 4; i++) {
            Particle p;
            init(p, 0.0);
            positions.push_back(p.position);
        }
        // Each spawned particle has zero velocity (initializer explicitly zeros it).
        // And distinct positions on the circle.
        CHECK(positions[0] != positions[1]);
        CHECK(positions[0] != positions[2]);
    }
}

TEST_SUITE("genParticleEmittOp dispatch") {
    TEST_CASE("boxrandom emitter name returns a working emit op") {
        wpscene::Emitter e;
        e.name        = "boxrandom";
        e.id          = 1;
        e.rate        = 2.0f;
        e.distancemin = { 0, 0, 0 };
        e.distancemax = { 0, 0, 0 };
        e.directions  = { 1, 1, 1 };
        e.origin      = { 0, 0, 0 };
        auto op       = WPParticleParser::genParticleEmittOp(e);
        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 0.5);
        CHECK(ps.size() >= 1u);
    }

    TEST_CASE("sphererandom emitter name returns a working emit op") {
        wpscene::Emitter e;
        e.name        = "sphererandom";
        e.id          = 1;
        e.rate        = 2.0f;
        e.distancemin = { 0, 0, 0 };
        e.distancemax = { 0, 0, 0 };
        e.directions  = { 1, 1, 1 };
        e.origin      = { 0, 0, 0 };
        e.sign        = { 0, 0, 0 };
        auto op       = WPParticleParser::genParticleEmittOp(e);
        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 0.5);
        CHECK(ps.size() >= 1u);
    }
}
