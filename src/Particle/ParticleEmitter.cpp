#include "ParticleEmitter.h"
#include "ParticleModify.h"
#include "Utils/Algorism.h"
#include "Utils/Logging.h"
#include "Utils/Algorism.h"
#include "Core/Random.hpp"

#include <Eigen/src/Core/Matrix.h>
#include <random>
#include <array>
#include <tuple>

using namespace wallpaper;

typedef std::function<Particle()> GenParticleOp;
typedef std::function<Particle()> SpwanOp;

namespace
{

inline std::tuple<u32, bool> FindLastParticle(std::span<const Particle> ps, u32 last) {
    for (u32 i = last; i < ps.size(); i++) {
        if (! ParticleModify::LifetimeOk(ps[i])) return { i, true };
    }
    return { 0, false };
}

inline u32 GetEmitNum(double& timer, float speed) {
    double emitDur = 1.0f / speed;
    if (emitDur > timer) return 0;
    u32 num = timer / emitDur;
    while (emitDur < timer) timer -= emitDur;
    if (timer < 0) timer = 0;
    return num;
}

inline u32 Emitt(std::vector<Particle>& particles, u32 num, u32 maxcount, bool sort,
                 SpwanOp Spwan) {
    u32  lastPartcle = 0;
    bool has_dead    = true;
    u32  i           = 0;

    for (i = 0; i < num; i++) {
        if (has_dead) {
            auto [r1, r2] = FindLastParticle(particles, lastPartcle);
            lastPartcle   = r1;
            has_dead      = r2;
        }
        if (has_dead) {
            particles[lastPartcle] = Spwan();

        } else {
            if (maxcount == particles.size()) break;
            particles.push_back(Spwan());
        }
    }

    if (sort) {
        // old << new << dead
        std::stable_sort(particles.begin(), particles.end(), [](const auto& a, const auto& b) {
            bool l_a = ParticleModify::LifetimeOk(a);
            bool l_b = ParticleModify::LifetimeOk(b);

            return (l_a && ! l_b) ||
                   (l_a && l_b && ! ParticleModify::IsNew(a) && ParticleModify::IsNew(b));
        });
    }

    return i + 1;
}

inline Particle Spwan(GenParticleOp gen, std::vector<ParticleInitOp>& inis, double duration) {
    auto particle = gen();
    for (auto& el : inis) el(particle, duration);
    return particle;
}

inline void ApplySign(Eigen::Vector3d& p, int32_t x, int32_t y, int32_t z) noexcept {
    if (x != 0) {
        p.x() = std::abs(p.x()) * (float)x;
    }
    if (y != 0) {
        p.y() = std::abs(p.y()) * (float)y;
    }
    if (z != 0) {
        p.z() = std::abs(p.z()) * (float)z;
    }
}
} // namespace

ParticleEmittOp ParticleBoxEmitterArgs::MakeEmittOp(ParticleBoxEmitterArgs a) {
    double timer { 0.0f };
    // Random initial phase so multiple emitters don't fire in sync
    double burstTimer { a.burstRate > 0 ? Random::get(0.0, (double)a.burstRate) : 0.0 };
    // Start exhausted — no emission until first burst fires
    u32 batchEmitted { a.burstRate > 0 ? a.batchSize : 0u };
    return [a, timer, burstTimer, batchEmitted](std::vector<Particle>&       ps,
                                                std::vector<ParticleInitOp>& inis,
                                                u32                          maxcount,
                                                double                       timepass) mutable {
        auto GenBox = [&]() {
            Eigen::Vector3d pos;
            for (int32_t i = 0; i < 3; i++)
                pos[i] = algorism::lerp(Random::get(-1.0, 1.0), a.minDistance[i], a.maxDistance[i]);
            auto p = Particle();
            pos    = pos.cwiseProduct(Eigen::Vector3f { a.directions.data() }.cast<double>());
            ParticleModify::MoveTo(p, pos);
            ParticleModify::ChangeVelocity(p,
                                           Random::get(a.minSpeed, a.maxSpeed) * pos.normalized());

            ParticleModify::Move(p, a.orgin[0], a.orgin[1], a.orgin[2]);
            return p;
        };

        u32 emit_num = 0;
        if (a.burstRate > 0) {
            // Burst mode: bolt grows at base rate, pauses between bursts
            burstTimer += timepass;
            if (burstTimer >= a.burstRate) {
                burstTimer -= a.burstRate;
                timer        = 0;
                batchEmitted = 0;
            }
            if (batchEmitted < a.batchSize) {
                timer += timepass;
                emit_num = GetEmitNum(timer, a.emitSpeed);
                // Cap to 1 per frame — bolt grows visibly, prevents zero-length segments
                if (emit_num > 1) emit_num = 1;
                // Cap to remaining batch
                u32 remaining = a.batchSize - batchEmitted;
                if (emit_num > remaining) emit_num = remaining;
                batchEmitted += emit_num;
            }
        } else {
            // Continuous mode
            timer += timepass;
            emit_num = GetEmitNum(timer, a.emitSpeed);
            // For rope: 1 per frame so bolt grows visibly
            if (a.batchSize > 1 || a.one_per_frame) {
                if (emit_num > 1) emit_num = 1;
            }
        }
        emit_num = a.instantaneous > 0 && ps.empty() ? a.instantaneous : emit_num;
        Emitt(ps, emit_num, maxcount, a.sort, [&]() {
            return Spwan(GenBox, inis, 1.0f / a.emitSpeed);
        });
    };
}

ParticleEmittOp ParticleSphereEmitterArgs::MakeEmittOp(ParticleSphereEmitterArgs a) {
    using namespace Eigen;
    double timer { 0.0f };
    // Random initial phase so multiple emitters don't fire in sync
    double burstTimer { a.burstRate > 0 ? Random::get(0.0, (double)a.burstRate) : 0.0 };
    // Start exhausted — no emission until first burst fires
    u32 batchEmitted { a.burstRate > 0 ? a.batchSize : 0u };
    return [a, timer, burstTimer, batchEmitted](std::vector<Particle>&       ps,
                                                std::vector<ParticleInitOp>& inis,
                                                u32                          maxcount,
                                                double                       timepass) mutable {
        auto GenSphere = [&]() {
            auto   p = Particle();
            double r = algorism::lerp(
                std::pow(Random::get(0.0, 1.0), 1.0 / 3.0), a.minDistance, a.maxDistance);
            Eigen::Vector3d sp = r * algorism::GenSphereSurfaceNormal(
                                         [](double u, double o) {
                                             return Random::get<std::normal_distribution<>>(u, o);
                                         },
                                         Eigen::Vector3f { a.directions.data() }.cast<double>());
            ApplySign(sp, a.sign[0], a.sign[1], a.sign[2]);

            ParticleModify::MoveTo(p, sp);
            ParticleModify::ChangeVelocity(p,
                                           Random::get(a.minSpeed, a.maxSpeed) * sp.normalized());

            ParticleModify::Move(p, Eigen::Vector3f { a.orgin.data() }.cast<double>());
            return p;
        };

        u32 emit_num = 0;
        if (a.burstRate > 0) {
            // Burst mode: bolt grows at base rate, pauses between bursts
            burstTimer += timepass;
            if (burstTimer >= a.burstRate) {
                burstTimer -= a.burstRate;
                timer        = 0;
                batchEmitted = 0;
            }
            if (batchEmitted < a.batchSize) {
                timer += timepass;
                emit_num = GetEmitNum(timer, a.emitSpeed);
                // Cap to 1 per frame — bolt grows visibly, prevents zero-length segments
                if (emit_num > 1) emit_num = 1;
                // Cap to remaining batch
                u32 remaining = a.batchSize - batchEmitted;
                if (emit_num > remaining) emit_num = remaining;
                batchEmitted += emit_num;
            }
        } else {
            // Continuous mode
            timer += timepass;
            emit_num = GetEmitNum(timer, a.emitSpeed);
            // For rope: 1 per frame so bolt grows visibly
            if (a.batchSize > 1 || a.one_per_frame) {
                if (emit_num > 1) emit_num = 1;
            }
        }
        emit_num = a.instantaneous > 0 && ps.empty() ? a.instantaneous : emit_num;
        Emitt(ps, emit_num, maxcount, a.sort, [&]() {
            return Spwan(GenSphere, inis, 1.0f / a.emitSpeed);
        });
    };
}
