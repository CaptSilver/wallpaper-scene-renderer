#pragma once
#include "ParticleEmitter.h"
#include "ParticleTrail.h"
#include "AudioRateMultiplier.hpp"
#include "Interface/IParticleRawGener.h"
#include "Core/NoCopyMove.hpp"
#include "Core/MapSet.hpp"

#include <atomic>
#include <memory>

namespace wallpaper
{

enum class ParticleAnimationMode
{
    SEQUENCE,
    RANDOMONE,
};

class ParticleSystem;

class ParticleInstance : NoCopy, NoMove {
public:
    struct BoundedData {
        ParticleInstance* parent { nullptr };
        isize             particle_idx { -1 };

        bool            pre_lifetime_ok { true };
        Eigen::Vector3f pos { 0.0f, 0.0f, 0.0f };
    };

    void Refresh();

    bool IsDeath() const;
    void SetDeath(bool);

    bool IsNoLiveParticle() const;
    void SetNoLiveParticle(bool);

    std::span<const Particle> Particles() const;
    std::vector<Particle>&    ParticlesVec();

    BoundedData&       GetBoundedData();
    const BoundedData& GetBoundedData() const { return m_bounded_data; }

    void                               InitTrails(u32 trail_capacity, float trail_max_age = 0.0f);
    std::vector<ParticleTrailHistory>& TrailHistories();
    u32                                TrailCapacity() const;

    // Resolves the parent-event particle for this instance, or nullptr when the link is
    // missing/stale/dead.  EVENT_FOLLOW, EVENT_SPAWN, and EVENT_DEATH children carry a
    // `parent` subsystem pointer + `particle_idx`; the helper guards each step (parent
    // pointer set, index in range, particle still alive) so callers can dereference the
    // returned pointer without re-checking.  Used by the inherit-from-event family
    // (initializer + per-frame operator) to read the parent's current color/size/alpha/
    // velocity/rotation, and by any future op that needs to bind to the parent.
    const Particle* GetEventParentParticle() const;

private:
    bool                              m_is_death { false };
    bool                              m_no_live_particle { false };
    std::vector<Particle>             m_particles;
    BoundedData                       m_bounded_data;
    std::vector<ParticleTrailHistory> m_trail_histories;
    u32                               m_trail_capacity { 0 };
    float                             m_trail_max_age { 0.0f };
};

class ParticleSubSystem : NoCopy, NoMove {
public:
    enum class SpawnType
    {
        STATIC,
        EVENT_FOLLOW,
        EVENT_SPAWN,
        EVENT_DEATH,
    };

public:
    ParticleSubSystem(ParticleSystem& p, std::shared_ptr<SceneMesh> sm, uint32_t maxcount,
                      double rate, u32 maxcount_instance, double probability, SpawnType type,
                      ParticleRawGenSpecOp specOp, float starttime = 0.0f);
    ~ParticleSubSystem();

    void Emitt();

    // Clears all particle instances and trail histories so the next Emitt()
    // treats the subsystem as freshly created — instantaneous emitters fire
    // again.  Used by the dynamic-asset pool to re-arm spent particle FX
    // (e.g. dino_run's coinget burst) when the script pops a new instance.
    void Reset();

    // True after the subsystem has had at least one live particle since the
    // last Reset() and all particles are now dead.  The dynamic-asset pool
    // uses this to auto-hide a burst-FX node once its effect has played out,
    // so scripts that don't promptly call destroyLayer don't leave lingering
    // slots visible on screen.
    bool IsBurstDone() const { return m_burst_done; }
    void ClearBurstDone() { m_burst_done = false; }

    // Runtime multiplier applied on top of the static `m_rate` factor.
    // Scripted instanceoverride.rate (NieR:Automata audio-reactive emission)
    // writes here once per property tick from the QML/scene-script thread;
    // Emitt() on the render thread reads it.  Defaults to 1.0 → no effect
    // when no script is driving it.  Atomic because render and script
    // threads are separate.
    void   SetDynamicRateMultiplier(double m) { m_dynamic_rate_multiplier.store(m); }
    double DynamicRateMultiplier() const { return m_dynamic_rate_multiplier.load(); }

    // Audio-reactive emit-rate scalar — pushed each render frame by
    // SceneWallpaper when this subsystem was flagged audio-reactive (any
    // emitter authored with a non-zero `audioprocessingmode`).  Independent
    // of the scripted dynamic multiplier so the two compose multiplicatively.
    void   SetAudioRateMultiplier(double m) { m_audio_rate_multiplier.store(m); }
    double AudioRateMultiplier() const { return m_audio_rate_multiplier.load(); }
    void   MarkAudioReactive() { m_audio_reactive = true; }
    bool   IsAudioReactive() const { return m_audio_reactive; }
    // Smoothing state held on the subsystem so attack/decay survives across
    // frames.  Touched only on the render thread (no atomic needed).
    double& AudioSmoothedRef() { return m_audio_smoothed; }

    // Per-emitter response parameters (channel mode + optional WE-shape curve
    // window / bounds / exponent / amount).  Populated at parse time from the
    // first audio-reactive emitter on this subsystem and read on the render
    // thread; not mutated post-load so no atomic is needed.
    void                                        SetAudioParams(const audio_reactive::RateMultiplierParams& p) { m_audio_params = p; }
    const audio_reactive::RateMultiplierParams& AudioParams() const { return m_audio_params; }

    ParticleInstance* QueryNewInstance();

    void AddEmitter(ParticleEmittOp&&);
    void AddInitializer(ParticleInitOp&&);
    void AddOperator(ParticleOperatorOp&&);

    void AddChild(std::unique_ptr<ParticleSubSystem>&&);

    ParticleSubSystem*       ParentSubsystem() { return m_parent_subsystem; }
    const ParticleSubSystem* ParentSubsystem() const { return m_parent_subsystem; }

    std::span<const ParticleControlpoint> Controlpoints() const;
    std::span<ParticleControlpoint>       Controlpoints();

    // OR `kCpReferencedFlag` into the runtime flags of CP slot `idx`.  Indices outside
    // [0..7] are clamped — the engine accepts any integer and silently maps it to the
    // nearest valid slot, so the marker behaves the same way.  Called once per CP
    // reference discovered while parsing operators / initializers.
    void MarkCpReferenced(int32_t idx);

    // Per-child CP shift authored on the parent's child block (`controlpointstartindex`
    // in the JSON).  Stored verbatim — the engine applies the shift at chain-resolve
    // time, layered on top of authored `parent_cp_index` rather than folded into it.
    // Slots whose authored `parent_cp_index` is 0 (the editor default) resolve through
    // `parent CP[slot_idx + shift]`; slots that authored an explicit non-zero value
    // route through that value as-is, so author intent always wins over the shift.
    void    SetCpStartShift(int32_t s) { m_cp_start_shift = s; }
    int32_t CpStartShift() const { return m_cp_start_shift; }

    // Resolve per-frame controlpoint offsets by walking the parent chain.  For CPs with
    // follow_parent_particle + an EVENT_FOLLOW/EVENT_SPAWN instance bound to a live parent
    // particle, the resolved offset = that particle's position.  For CPs with
    // parent_cp_index > 0, the resolved offset = parent subsystem's CP[parent_cp_index].resolved
    // (parent has already been resolved this frame because Emitt runs top-down).  Otherwise the
    // resolved offset falls back to the static local `offset`.
    //
    // `inst` may be null for subsystems without bounded instances (STATIC top-level).
    void ResolveControlpointsForInstance(const ParticleInstance* inst);

    // Update control points with link_mouse flag to follow mouse position
    void UpdateMouseControlPoints(double sceneX, double sceneY);

    SpawnType Type() const;
    u32       MaxInstanceCount() const;

    void SetSpriteTrail(u32 trail_capacity, float trail_length = 0.0f);

    // Debug label (particle preset basename, e.g. "thunderbolt_glow") plumbed from the
    // parser.  Used only for rate-limited LOG_INFO diagnostics when investigating
    // stickiness / clear events.  Empty string = unnamed (top-level from scene.json).
    void               SetDebugName(std::string name) { m_debug_name = std::move(name); }
    const std::string& DebugName() const { return m_debug_name; }

    // WE semantics: `starttime` is the number of seconds of simulation the
    // particle system is advanced BEFORE the first rendered frame, so
    // particles are already at steady-state distribution on frame 1.  Used
    // by ParticleSystem::PreSimulate().
    float StartTime() const { return m_starttime; }
    void  ClearStartTime() { m_starttime = 0.0f; }

    const std::vector<std::unique_ptr<ParticleSubSystem>>& Children() const {
        return m_children;
    }

private:
    ParticleSystem&            m_sys;
    std::shared_ptr<SceneMesh> m_mesh;
    //	std::vector<std::unique_ptr<ParticleEmitter>> m_emiters;
    std::vector<ParticleEmittOp> m_emiters;

    // std::vector<Particle>           m_particles;
    std::vector<ParticleInitOp>     m_initializers;
    std::vector<ParticleOperatorOp> m_operators;

    std::array<ParticleControlpoint, 8> m_controlpoints;

    ParticleRawGenSpecOp m_genSpecOp;
    u32                  m_maxcount;
    float                m_starttime;
    double               m_rate;
    double               m_time;

    std::vector<std::unique_ptr<ParticleSubSystem>> m_children;
    ParticleSubSystem* m_parent_subsystem { nullptr };  // set in parent's AddChild()
    std::vector<std::unique_ptr<ParticleInstance>>  m_instances;

    u32       m_maxcount_instance { 1 };
    double    m_probability { 1.0f };
    SpawnType m_spawn_type { SpawnType::STATIC };

    bool  m_is_spritetrail { false };
    u32   m_trail_capacity { 0 };
    float m_trail_length { 0.0f };

    // Tracks burst-FX completion for the dynamic-asset pool.  Set true by
    // Emitt() when the subsystem has had at least one live particle since
    // the last Reset() but currently has none.
    bool m_burst_done { false };
    bool m_any_alive_since_reset { false };

    // Scripted rate override — see SetDynamicRateMultiplier.
    std::atomic<double> m_dynamic_rate_multiplier { 1.0 };

    // Audio-reactive rate override — see SetAudioRateMultiplier.  Only ever
    // pushed when m_audio_reactive is true; otherwise stays at 1.0 (no-op).
    std::atomic<double>                  m_audio_rate_multiplier { 1.0 };
    double                               m_audio_smoothed { 0.0 };
    bool                                 m_audio_reactive { false };
    audio_reactive::RateMultiplierParams m_audio_params {};

    // Stored CP-slot shift authored on the parent's child block; consumed by the chain
    // resolver instead of being baked into per-CP `parent_cp_index` at parse time.
    int32_t m_cp_start_shift { 0 };

    std::string m_debug_name;
};

class Scene;
class ParticleSystem : NoCopy, NoMove {
public:
    ParticleSystem(Scene& scene): scene(scene) {};
    ~ParticleSystem() = default;

    void Emitt();

    // Update control points that have link_mouse flag set
    // mousePos: normalized mouse position (0-1), orthoSize: scene dimensions
    void UpdateMouseControlPoints(const std::array<float, 2>& mousePos,
                                  const std::array<int, 2>&   orthoSize);

    // Max `starttime` across every subsystem in this scene (recursive into
    // children).  0 for particle-less scenes and for scenes whose authors
    // didn't set starttime.  Drives PreSimulate()'s iteration budget.
    double MaxStartTime() const;

    // Run the scene's particle systems forward by MaxStartTime() seconds so
    // each subsystem is at steady-state distribution before the first
    // rendered frame.  Required for WE "starttime" semantics — e.g. the
    // shipped shimmering_particles default needs 50s (dustmotes) / 200s
    // (small_motes) of pre-sim, otherwise the user sees a black screen
    // waiting for particle emission to ramp up.
    //
    // Uses the scene's own clock (`scene.elapsingTime` / `scene.frameTime`)
    // so per-subsystem `m_starttime` gates stagger correctly during the
    // pre-sim.  Afterwards, each subsystem's `m_starttime` is cleared and
    // the scene clock is rewound so rendering sees t=0 with particles
    // already distributed.
    //
    // `dt` is the pre-sim step in seconds; defaults to the 32ms per-frame
    // cap Emitt() already enforces, so this is "run the same tick the
    // renderer would have run, minus the draw".  No-op when MaxStartTime
    // is 0 (the overwhelming majority of wallpapers).
    void PreSimulate(double dt = 0.032);

    Scene& scene;

    std::vector<std::unique_ptr<ParticleSubSystem>> subsystems;
    std::unique_ptr<IParticleRawGener>              gener;
};
} // namespace wallpaper
