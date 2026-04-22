#pragma once
#include "ParticleEmitter.h"
#include "ParticleTrail.h"
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

    BoundedData& GetBoundedData();

    void                               InitTrails(u32 trail_capacity, float trail_max_age = 0.0f);
    std::vector<ParticleTrailHistory>& TrailHistories();
    u32                                TrailCapacity() const;

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
                      ParticleRawGenSpecOp specOp, uint32_t starttime = 0);
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

    ParticleInstance* QueryNewInstance();

    void AddEmitter(ParticleEmittOp&&);
    void AddInitializer(ParticleInitOp&&);
    void AddOperator(ParticleOperatorOp&&);

    void AddChild(std::unique_ptr<ParticleSubSystem>&&);

    std::span<const ParticleControlpoint> Controlpoints() const;
    std::span<ParticleControlpoint>       Controlpoints();

    // Update control points with link_mouse flag to follow mouse position
    void UpdateMouseControlPoints(double sceneX, double sceneY);

    SpawnType Type() const;
    u32       MaxInstanceCount() const;

    void SetSpriteTrail(u32 trail_capacity, float trail_length = 0.0f);

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
    u32                  m_starttime;
    double               m_rate;
    double               m_time;

    std::vector<std::unique_ptr<ParticleSubSystem>> m_children;
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

    Scene& scene;

    std::vector<std::unique_ptr<ParticleSubSystem>> subsystems;
    std::unique_ptr<IParticleRawGener>              gener;
};
} // namespace wallpaper
