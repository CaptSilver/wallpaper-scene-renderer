#pragma once

// Shared fixture builders for the two WPParticleParser residual mutant-kill
// suites (test_WPParticleParserResidual.cpp / Residual2.cpp).  Both files
// build identical Particle/override/controlpoint fixtures and run the same
// init/operator harness against them; this header is the one copy.

#include "WPParticleParser.hpp"
#include "Particle/Particle.h"
#include "Particle/ParticleEmitter.h"
#include "Particle/ParticleModify.h"
#include "Particle/ParticleSystem.h"
#include "wpscene/WPParticleObject.h"

#include <Eigen/Core>
#include <span>
#include <vector>

namespace wallpaper
{
namespace test_fixtures
{

inline Particle makeParticle() {
    Particle p;
    p.lifetime        = 1.0f;
    p.init.lifetime   = 1.0f;
    p.alpha           = 1.0f;
    p.init.alpha      = 1.0f;
    p.size            = 20.0f;
    p.init.size       = 20.0f;
    p.color           = Eigen::Vector3f(1, 1, 1);
    p.init.color      = p.color;
    p.position        = Eigen::Vector3f(0, 0, 0);
    p.velocity        = Eigen::Vector3f(0, 0, 0);
    p.rotation        = Eigen::Vector3f(0, 0, 0);
    p.angularVelocity = Eigen::Vector3f(0, 0, 0);
    p.random_seed     = 0xdeadbeef;
    return p;
}

inline wpscene::ParticleInstanceoverride emptyOverride() {
    wpscene::ParticleInstanceoverride o;
    o.enabled = false;
    return o;
}

inline std::span<ParticleControlpoint> defaultCps(std::vector<ParticleControlpoint>& storage) {
    storage.assign(8, ParticleControlpoint {});
    for (auto& cp : storage) {
        cp.resolved = Eigen::Vector3d(0, 0, 0);
        cp.velocity = Eigen::Vector3d(0, 0, 0);
    }
    return std::span<ParticleControlpoint>(storage.data(), storage.size());
}

inline void runInit(const ParticleInitOp& init, Particle& p, double dt = 0.0) { init(p, dt); }

inline Particle runOpSingle(ParticleOperatorOp op, const Particle& base, double time_pass = 0.016,
                            double time = 0.0) {
    std::vector<Particle> ps;
    ps.reserve(8);
    ps.push_back(base);
    std::vector<ParticleControlpoint> cps_storage;
    auto                              cps = defaultCps(cps_storage);
    ParticleInfo                      info {
        .particles     = std::span<Particle>(ps),
        .controlpoints = cps,
        .time          = time,
        .time_pass     = time_pass,
    };
    op(info);
    return ps[0];
}

inline Particle runOpSingleCps(ParticleOperatorOp op, const Particle& base,
                               std::span<ParticleControlpoint> cps, double time_pass = 0.016,
                               double time = 0.0) {
    std::vector<Particle> ps;
    ps.reserve(8);
    ps.push_back(base);
    ParticleInfo info {
        .particles     = std::span<Particle>(ps),
        .controlpoints = cps,
        .time          = time,
        .time_pass     = time_pass,
    };
    op(info);
    return ps[0];
}

} // namespace test_fixtures
} // namespace wallpaper
