#include "ParticleSystem.h"
#include "Core/Literals.hpp"
#include "Scene/Scene.h"
#include "ParticleModify.h"
#include "Scene/SceneMesh.h"
#include "Core/Random.hpp"

#include "Utils/Logging.h"

#include <algorithm>

using namespace wallpaper;

void ParticleInstance::Refresh() {
    SetDeath(false);
    SetNoLiveParticle(false);
    GetBoundedData() = {};
    ParticlesVec().clear();
    for (auto& trail : m_trail_histories) trail.Clear();
}

void ParticleInstance::InitTrails(u32 trail_capacity, float trail_max_age) {
    m_trail_capacity = trail_capacity;
    m_trail_max_age  = trail_max_age;
}

std::vector<ParticleTrailHistory>& ParticleInstance::TrailHistories() { return m_trail_histories; }

u32 ParticleInstance::TrailCapacity() const { return m_trail_capacity; }

bool ParticleInstance::IsDeath() const { return m_is_death; }
void ParticleInstance::SetDeath(bool v) { m_is_death = v; };

bool ParticleInstance::IsNoLiveParticle() const { return m_no_live_particle; };
void ParticleInstance::SetNoLiveParticle(bool v) { m_no_live_particle = v; };

std::span<const Particle> ParticleInstance::Particles() const { return m_particles; };
std::vector<Particle>&    ParticleInstance::ParticlesVec() { return m_particles; };

ParticleInstance::BoundedData& ParticleInstance::GetBoundedData() { return m_bounded_data; }

ParticleSubSystem::ParticleSubSystem(ParticleSystem& p, std::shared_ptr<SceneMesh> sm,
                                     uint32_t maxcount, double rate, u32 maxcount_instance,
                                     double probability, SpawnType type,
                                     ParticleRawGenSpecOp specOp, uint32_t starttime)
    : m_sys(p),
      m_mesh(sm),
      m_maxcount(maxcount),
      m_starttime(starttime),
      m_rate(rate),
      m_genSpecOp(specOp),
      m_time(0),
      m_maxcount_instance(maxcount_instance),
      m_probability(probability),
      m_spawn_type(type) {};

ParticleSubSystem::~ParticleSubSystem() = default;

void ParticleSubSystem::AddEmitter(ParticleEmittOp&& em) { m_emiters.emplace_back(em); }

void ParticleSubSystem::AddInitializer(ParticleInitOp&& ini) { m_initializers.emplace_back(ini); }

void ParticleSubSystem::AddOperator(ParticleOperatorOp&& op) { m_operators.emplace_back(op); }

std::span<const ParticleControlpoint> ParticleSubSystem::Controlpoints() const {
    return m_controlpoints;
}
std::span<ParticleControlpoint> ParticleSubSystem::Controlpoints() { return m_controlpoints; };

ParticleSubSystem::SpawnType ParticleSubSystem::Type() const { return m_spawn_type; }

u32 ParticleSubSystem::MaxInstanceCount() const { return m_maxcount_instance; };

void ParticleSubSystem::SetSpriteTrail(u32 trail_capacity, float trail_length) {
    m_is_spritetrail = true;
    m_trail_capacity = trail_capacity;
    m_trail_length   = trail_length;
}

void ParticleSubSystem::AddChild(std::unique_ptr<ParticleSubSystem>&& child) {
    m_children.emplace_back(std::move(child));
}

void ParticleSubSystem::Reset() {
    // Rearm every instance so emitters with instantaneous>0 see ps.empty()
    // next frame and re-fire their burst.  Also reset the subsystem clock so
    // periodic / delayed emitters restart from time zero.
    // Refresh() clears the particle vector, trail histories, bounded-data
    // link, and flips death/no-live-particle flags back to false — exactly
    // the state we need for the emitter to treat this like a fresh spawn.
    for (auto& inst : m_instances) {
        if (inst) inst->Refresh();
    }
    m_time                  = 0.0;
    m_burst_done            = false;
    m_any_alive_since_reset = false;
    for (auto& child : m_children) {
        if (child) child->Reset();
    }
}

ParticleInstance* ParticleSubSystem::QueryNewInstance() {
    if (Random::get(0.0, 1.0) <= m_probability) {
        for (auto& inst : m_instances) {
            if (inst->IsDeath() && inst->IsNoLiveParticle()) {
                inst->Refresh();
                return inst.get();
            }
        }
        if (m_instances.size() < m_maxcount_instance) {
            auto& inst = m_instances.emplace_back(std::make_unique<ParticleInstance>());
            if (m_is_spritetrail) {
                inst->InitTrails(m_trail_capacity, m_trail_length);
            }
            return inst.get();
        }
    }
    return nullptr;
}

void ParticleSubSystem::Emitt() {
    if (m_sys.scene.elapsingTime < (double)m_starttime) return;

    // Cap per-frame particle time to avoid burst emission when the frame is
    // slow (e.g. first frame after scene compile, or when fps target is low
    // like 15 — a 66ms frame otherwise emits 33 particles at once, visible
    // as a pulse of new stars on screen).  A cap of 32ms spreads bursts over
    // at most two render ticks at 60fps target.
    constexpr double kMaxParticleFrameTime = 0.032;
    double           frameTime             = std::min(m_sys.scene.frameTime, kMaxParticleFrameTime);
    // Dynamic multiplier is 1.0 when no script is driving this subsystem.
    // NieR:Automata's audio-reactive starfield writes 0.1..1.0 here each
    // property tick based on bass amplitude.
    double rate_eff     = m_rate * m_dynamic_rate_multiplier.load();
    double particleTime = frameTime * rate_eff;
    m_time += particleTime;

    if (m_spawn_type == SpawnType::STATIC) {
        if (m_instances.empty()) {
            auto& inst = m_instances.emplace_back(std::make_unique<ParticleInstance>());
            if (m_is_spritetrail) {
                inst->InitTrails(m_trail_capacity, m_trail_length);
            }
        }
    }

    auto spawn_inst = [](ParticleInstance& inst, ParticleSubSystem& child, isize idx) {
        ParticleInstance* n_inst = child.QueryNewInstance();
        if (n_inst != nullptr) {
            n_inst->GetBoundedData() = {
                .parent       = &inst,
                .particle_idx = idx,
            };
        }
    };

    for (auto& inst : m_instances) {
        assert(inst);

        auto& bounded_data = inst->GetBoundedData();

        bool type_has_death =
            m_spawn_type == SpawnType::EVENT_SPAWN || m_spawn_type == SpawnType::EVENT_FOLLOW;

        // bouded data and death
        if (bounded_data.parent != nullptr) {
            std::span particles = bounded_data.parent->Particles();
            if (bounded_data.particle_idx != -1 && bounded_data.particle_idx < particles.size()) {
                auto& p          = particles[bounded_data.particle_idx];
                bounded_data.pos = ParticleModify::GetPos(p);
                // only update pos once when event_death
                if (m_spawn_type == SpawnType::EVENT_DEATH) bounded_data.particle_idx = -1;

                // death if bounded particle death
                if (! inst->IsDeath() && type_has_death) {
                    bool cur_life_ok = ParticleModify::LifetimeOk(p);
                    inst->SetDeath(! cur_life_ok && bounded_data.pre_lifetime_ok);
                    bounded_data.pre_lifetime_ok = cur_life_ok;
                }
            }

            // death if parent death
            if (! inst->IsDeath() && type_has_death) {
                inst->SetDeath(bounded_data.parent->IsDeath());
            }
        }

        // clear when death if follow
        if (inst->IsDeath() && m_spawn_type == SpawnType::EVENT_FOLLOW) {
            inst->ParticlesVec().clear();
        }

        if (! inst->IsDeath()) {
            for (auto& emittOp : m_emiters) {
                emittOp(inst->ParticlesVec(), m_initializers, m_maxcount, particleTime);
            }
        }

        // event_death is death when no live particles left
        if (m_spawn_type == SpawnType::EVENT_DEATH && inst->IsNoLiveParticle() &&
            ! inst->ParticlesVec().empty()) {
            inst->SetDeath(true);
        }

        ParticleInfo info {
            .particles     = inst->ParticlesVec(),
            .controlpoints = m_controlpoints,
            .time          = m_time,
            .time_pass     = particleTime,
        };

        bool  has_live = false;
        isize i        = -1;
        for (auto& p : info.particles) {
            i++;

            bool is_new = ParticleModify::IsNew(p);
            if (is_new) {
                // Slot may have been reused from a dead particle — wipe the old
                // trail so the new particle doesn't inherit a streak from the
                // previous life's final position.  Without this, reincarnated
                // particles render as giant streaks from old position to new.
                if (m_is_spritetrail && i < (isize)inst->TrailHistories().size()) {
                    inst->TrailHistories()[i].Clear();
                }
                // new spawn
                for (auto& child : m_children) {
                    if (child->Type() == SpawnType::EVENT_FOLLOW ||
                        child->Type() == SpawnType::EVENT_SPAWN)
                        spawn_inst(*inst, *child, i);
                }
            }

            ParticleModify::MarkOld(p);
            if (! ParticleModify::LifetimeOk(p)) {
                // Mark newly-dead particles so trail recording can clear them
                if (m_is_spritetrail && i < (isize)inst->TrailHistories().size()) {
                    inst->TrailHistories()[i].Clear();
                }
                continue;
            }
            ParticleModify::Reset(p);
            ParticleModify::ChangeLifetime(p, -particleTime);

            if (! ParticleModify::LifetimeOk(p)) {
                // new dead
                for (auto& child : m_children) {
                    if (child->Type() == SpawnType::EVENT_DEATH) spawn_inst(*inst, *child, i);
                }
            } else {
                has_live = true;
            }
        }

        inst->SetNoLiveParticle(! has_live);

        std::for_each(m_operators.begin(), m_operators.end(), [&info](ParticleOperatorOp& op) {
            op(info);
        });

        // Record trail positions for spritetrail particles
        if (m_is_spritetrail) {
            auto& trails    = inst->TrailHistories();
            auto& particles = inst->ParticlesVec();
            // Grow trail history vector to match particle count
            while (trails.size() < particles.size()) {
                trails.emplace_back();
                trails.back().Init(m_trail_capacity, m_trail_length);
            }
            usize alive_count = 0;
            usize trail_count = 0;
            for (usize pi = 0; pi < particles.size(); pi++) {
                auto& p     = particles[pi];
                auto& trail = trails[pi];
                if (ParticleModify::LifetimeOk(p)) {
                    float cur_time = (float)m_sys.scene.elapsingTime;
                    trail.Push({ p.position, p.size, p.alpha, p.color, cur_time });
                    alive_count++;
                    if (trail.Count() >= 2) trail_count++;
                }
                // Dead particles already had trail cleared in the loop above
            }
            static int s_trail_log_counter = 0;
            if (++s_trail_log_counter % 6000 == 1 && alive_count > 0) {
                LOG_INFO("spritetrail: alive=%zu renderable=%zu capacity=%u particles=%zu",
                         alive_count,
                         trail_count,
                         m_trail_capacity,
                         particles.size());
            }
        }
    }

    // Burst-completion tracking for the dynamic-asset pool: once a subsystem
    // has had particles alive since its last Reset() and all instances now
    // report no live particles, flag m_burst_done so the render thread can
    // auto-hide the owning node.  Without this, burst FX (e.g. dino_run's
    // coin-pickup sparkle) would leave their node visible indefinitely after
    // particles die — a non-issue visually (dead particles skip rendering)
    // but it keeps pool slots "out" until the script explicitly destroys
    // them, which some wallpapers forget to do.
    bool any_alive = false;
    for (auto& inst : m_instances) {
        if (inst && ! inst->IsNoLiveParticle()) {
            any_alive = true;
            break;
        }
    }
    if (any_alive)
        m_any_alive_since_reset = true;
    else if (m_any_alive_since_reset && ! m_burst_done)
        m_burst_done = true;

    // Spawner-only particles have no vertex arrays — skip render data generation
    if (m_mesh->VertexCount() > 0) {
        m_mesh->SetDirty();
        m_sys.gener->GenGLData(m_instances, *m_mesh, m_genSpecOp);
    }

    for (auto& child : m_children) {
        child->Emitt();
    }
}

void ParticleSystem::Emitt() {
    static int s_ps_log = 0;
    if (++s_ps_log % 600 == 1) {
        LOG_INFO("ParticleSystem::Emitt: %zu subsystems, elapsed=%f",
                 subsystems.size(),
                 scene.elapsingTime);
    }
    for (auto& el : subsystems) {
        el->Emitt();
    }
}

void ParticleSubSystem::UpdateMouseControlPoints(double sceneX, double sceneY) {
    for (auto& cp : m_controlpoints) {
        if (cp.link_mouse) {
            cp.offset = Eigen::Vector3d(sceneX, sceneY, 0.0);
        }
    }
    // Update children recursively
    for (auto& child : m_children) {
        child->UpdateMouseControlPoints(sceneX, sceneY);
    }
}

void ParticleSystem::UpdateMouseControlPoints(const std::array<float, 2>& mousePos,
                                              const std::array<int, 2>&   orthoSize) {
    // Convert normalized mouse position (0-1) to scene coordinates
    // mousePos is in range [0,1] where (0,0) is top-left and (1,1) is bottom-right
    // Scene coordinates have origin at center, so we need to convert
    double sceneX = (mousePos[0] - 0.5) * orthoSize[0];
    double sceneY = (0.5 - mousePos[1]) * orthoSize[1]; // Flip Y axis

    for (auto& subsys : subsystems) {
        subsys->UpdateMouseControlPoints(sceneX, sceneY);
    }
}
