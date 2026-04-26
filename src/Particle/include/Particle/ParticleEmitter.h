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

class ParticleInstance;

// Bit OR-ed into a runtime CP's `runtime_flags` once the parser has seen any operator
// that names that CP slot.  The slot table is exactly 8 entries and the CP-index range
// any operator may name is [0..7], so the bit lives well above any authored low-byte
// semantics.  Defined here (rather than in the JSON-coupled WPMapSequenceParse.hpp) so
// the runtime particle library doesn't pick up a JSON dep just to read the constant.
constexpr uint32_t kCpReferencedFlag = 0x10000u;

// Largest valid CP slot index.  The runtime CP table has exactly 8 entries (0..7); any
// authored value beyond the table clamps down to the highest valid slot.
constexpr int32_t kMaxCpIndex = 7;

inline int32_t ClampCpIndex(int32_t v) {
    if (v < 0) return 0;
    if (v > kMaxCpIndex) return kMaxCpIndex;
    return v;
}

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

    uint32_t runtime_flags { 0 };
    // Parser-derived bitfield about how the runtime should treat this slot.  Today only
    // bit `kCpReferencedFlag` (0x10000) is consumed: the parser ORs it into every CP
    // any operator names so resolver fast-paths can skip slots no operator will read.
    // Authored CP `flags` (link_mouse, worldspace, follow_parent_particle) still live in
    // their own bools above — `runtime_flags` is intentionally distinct so the boundary
    // between authored and parser-derived bits stays explicit.

    // Per-frame derived velocity of the resolved CP position.  `prev_resolved` is the
    // value stored at the END of the previous resolve pass; `velocity` is
    // `(resolved - prev_resolved) / dt` computed each frame.  Used by the
    // `inheritcontrolpointvelocity` initializer (children spawn with a velocity
    // matching their CP's motion) and the universal `remapvalue` operator (input =
    // controlpointvelocity).  Zero on the first frame after spawn — there is no
    // history to differentiate.
    Eigen::Vector3d prev_resolved { 0, 0, 0 };
    Eigen::Vector3d velocity { 0, 0, 0 };
    bool            has_prev_resolved { false };
};

struct ParticleInfo {
    std::span<Particle>                   particles;
    std::span<const ParticleControlpoint> controlpoints;
    double                                time;
    double                                time_pass;
    // Per-instance back-pointer.  Operators that need to dereference the parent
    // particle (event-bound subsystems) read it through `instance->GetEventParentParticle()`.
    // Null on STATIC top-level subsystems and on instances with no live bound parent.
    const ParticleInstance*               instance { nullptr };
};

using ParticleInitOp = std::function<void(Particle&, double)>;
// particle index lifetime-percent passTime
using ParticleOperatorOp = std::function<void(const ParticleInfo&)>;

using ParticleEmittOp = std::function<void(std::vector<Particle>&, std::vector<ParticleInitOp>&,
                                           uint32_t maxcount, double timepass)>;

// Per-spawn back-pointer so initializers that need parent-event context can dereference
// the currently-spawning ParticleInstance without an explicit signature change on
// ParticleInitOp.  Set by ParticleSubSystem::Emitt before invoking each instance's
// emitters and cleared after, so reads outside the emit-loop see nullptr.  The pointer
// is thread-local; every particle-system call path runs on the render thread, so the
// cell is shared across emit ops within one tick and isolated from the script thread
// (where atomic dynamic-rate writes live, but no emit traffic).
//
// Used by `inheritinitialvaluefromevent` to copy the parent particle's color / size /
// alpha / velocity / rotation / lifetime into the child at spawn.
namespace particle_spawn_context
{
void                    SetSpawnInstance(const ParticleInstance* inst);
const ParticleInstance* CurrentSpawnInstance();
} // namespace particle_spawn_context

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
