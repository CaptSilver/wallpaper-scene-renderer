#pragma once
#include "Particle.h"

#include <vector>
#include <random>
#include <memory>
#include <functional>
#include <array>
#include <span>

#include "Core/Literals.hpp"

namespace wallpaper
{

struct ParticleControlpoint {
    bool            link_mouse { false };
    bool            worldspace { false };
    // Parent-chain fields (see memory/cp-parent-chain.md).  Dormant on ~99.7% of CPs in real
    // workshop assets; only fire when the scene author set non-default values.
    bool follow_parent_particle { false };
    // Dynamic bind: resolved offset = parent subsystem's bound particle position each frame.
    int32_t parent_cp_index { 0 };
    // Static chain: resolved offset = parent subsystem's controlpoints[parent_cp_index].resolved.
    // 0 = no chain (WE convention).
    Eigen::Vector3d offset { 0, 0, 0 };
    // Static local offset — the base value when no chain/bind is active.
    Eigen::Vector3d resolved { 0, 0, 0 };
    // Per-frame output of the chain resolver.  Operators/initializers read THIS, not offset.
    // Defaults to offset so pre-resolve reads remain correct for non-chaining CPs.
    bool is_null_offset { false };
    // True when the source JSON had `offset: null` (uninitialised CP slot in the WE editor)
    // AND no instance override.  Used by ResolveControlpointsForInstance to substitute a
    // bounded parent-particle position instead of (0,0,0) for the NieR 2B
    // thunderbolt_beam_child case: beam_child's CP1 chains via controlpointstartindex=1 to
    // spawner CP2, which has `offset: null` in the preset.  Without substitution the rope
    // draws from the bolt particle back to the subsystem origin (user report: "goes back to
    // hit that").  With substitution, CP1 resolves to beam_child's bounded spawner particle,
    // so the sub-bolt correctly branches from bolt particle to its spawner spark.
    bool is_null_resolved { false };
    // Propagated through chain resolution: true when this CP's resolved value came from a
    // chain that ultimately terminated in a null-offset CP (or is itself null-offset).
    // Lets ancestors still see the "null" flag after a multi-hop chain.
    Eigen::Vector3d angles { 0, 0, 0 };
    // Per-CP orientation (radians, XYZ Euler).  Populated from
    // `instanceoverride.controlpointangle[N]` at scene-load time.  Currently written but
    // not read by any runtime operator — see WPSceneParser::LoadControlPoint and
    // memory/nier-2b-wallpaper.md for the "plumb through, log loudly" rationale.
};

struct ParticleInfo {
    std::span<Particle>                   particles;
    std::span<const ParticleControlpoint> controlpoints;
    double                                time;
    double                                time_pass;
};

using ParticleInitOp = std::function<void(Particle&, double)>;
// particle index lifetime-percent passTime
using ParticleOperatorOp = std::function<void(const ParticleInfo&)>;

using ParticleEmittOp = std::function<void(std::vector<Particle>&, std::vector<ParticleInitOp>&,
                                           uint32_t maxcount, double timepass)>;

struct ParticleBoxEmitterArgs {
    std::array<float, 3> directions;
    std::array<float, 3> minDistance;
    std::array<float, 3> maxDistance;
    float                emitSpeed;
    std::array<float, 3> orgin;
    bool                 one_per_frame;
    bool                 sort;
    u32                  instantaneous;
    float                minSpeed;
    float                maxSpeed;
    u32                  batchSize { 1 };
    float                burstRate { 0.0f };

    static ParticleEmittOp MakeEmittOp(ParticleBoxEmitterArgs);
};

struct ParticleSphereEmitterArgs {
    std::array<float, 3>   directions;
    float                  minDistance;
    float                  maxDistance;
    float                  emitSpeed;
    std::array<float, 3>   orgin;
    std::array<int32_t, 3> sign;
    bool                   one_per_frame;
    bool                   sort;
    u32                    instantaneous;
    float                  minSpeed;
    float                  maxSpeed;
    u32                    batchSize { 1 };
    float                  burstRate { 0.0f };

    static ParticleEmittOp MakeEmittOp(ParticleSphereEmitterArgs);
};

} // namespace wallpaper
