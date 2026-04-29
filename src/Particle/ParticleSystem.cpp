#include "ParticleSystem.h"
#include "Core/Literals.hpp"
#include "Scene/Scene.h"
#include "ParticleModify.h"
#include "Scene/SceneMesh.h"
#include "Core/Random.hpp"

#include "Utils/Logging.h"

#include <algorithm>

using namespace wallpaper;

namespace
{
thread_local const ParticleInstance* tl_spawn_instance { nullptr };
}

namespace wallpaper::particle_spawn_context
{
void                    SetSpawnInstance(const ParticleInstance* inst) { tl_spawn_instance = inst; }
const ParticleInstance* CurrentSpawnInstance() { return tl_spawn_instance; }
} // namespace wallpaper::particle_spawn_context

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

const Particle* ParticleInstance::GetEventParentParticle() const {
    if (m_bounded_data.parent == nullptr) return nullptr;
    if (m_bounded_data.particle_idx < 0) return nullptr;
    auto parent_particles = m_bounded_data.parent->Particles();
    if (static_cast<usize>(m_bounded_data.particle_idx) >= parent_particles.size()) return nullptr;
    const Particle& p = parent_particles[static_cast<usize>(m_bounded_data.particle_idx)];
    if (p.lifetime <= 0.0f) return nullptr;
    return &p;
}

ParticleSubSystem::ParticleSubSystem(ParticleSystem& p, std::shared_ptr<SceneMesh> sm,
                                     uint32_t maxcount, double rate, u32 maxcount_instance,
                                     double probability, SpawnType type,
                                     ParticleRawGenSpecOp specOp, float starttime)
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

void ParticleSubSystem::MarkCpReferenced(int32_t idx) {
    int32_t clamped = ClampCpIndex(idx);
    m_controlpoints[static_cast<size_t>(clamped)].runtime_flags |= kCpReferencedFlag;
}

ParticleSubSystem::SpawnType ParticleSubSystem::Type() const { return m_spawn_type; }

u32 ParticleSubSystem::MaxInstanceCount() const { return m_maxcount_instance; };

void ParticleSubSystem::SetSpriteTrail(u32 trail_capacity, float trail_length) {
    m_is_spritetrail = true;
    m_trail_capacity = trail_capacity;
    m_trail_length   = trail_length;
}

void ParticleSubSystem::AddChild(std::unique_ptr<ParticleSubSystem>&& child) {
    child->m_parent_subsystem = this;
    m_children.emplace_back(std::move(child));
}

void ParticleSubSystem::ResolveControlpointsForInstance(const ParticleInstance* inst) {
    // Order of precedence:
    //   1. follow_parent_particle  → live bound particle position (requires inst with parent)
    //   2. parent_cp_index > 0     → parent subsystem's already-resolved CP (top-down update
    //                                guarantees parent is resolved first)
    //   3. static local offset
    //
    // When follow_parent_particle is set but no parent particle is bound (STATIC root, or the
    // instance is a fresh STATIC default), we fall through to the chain branch so the author's
    // intended fallback still applies (NieR thunderbolt_child_spawner CP1 has both).
    //
    // Null-offset substitution: if a CP's resolved value ultimately comes from a JSON
    // `offset: null` slot (explicitly unassigned by the author in the WE editor), and this
    // instance has a bounded parent particle, substitute the bounded particle position.
    // Fixes NieR 2B thunderbolt_beam_child: beam_child's CP1 chains via controlpointstartindex=1
    // to spawner CP2 (offset:null in preset).  Without substitution the rope draws from the
    // bolt particle back to subsystem origin ("goes back to hit that").  With substitution,
    // CP1 resolves to beam_child's bounded spawner particle, so sub-bolts branch from bolt
    // particle to spawner spark.
    const bool has_bound_particle = (inst != nullptr && inst->GetBoundedData().parent != nullptr &&
                                     inst->GetBoundedData().particle_idx >= 0);
    Eigen::Vector3d bound_pos = Eigen::Vector3d::Zero();
    if (has_bound_particle) bound_pos = inst->GetBoundedData().pos.cast<double>();
    for (size_t slot = 0; slot < m_controlpoints.size(); ++slot) {
        auto& cp                 = m_controlpoints[slot];
        bool  resolved_is_null   = false;
        // Effective parent CP index = authored value when the author chose one, else
        // the slot's own index plus the child-container shift.  This keeps the
        // authored value pristine on the CP record (the WE storage model) while
        // still routing default-0 slots through the shift the parent's child block
        // requested.
        int32_t effective_parent = cp.parent_cp_index;
        if (effective_parent == 0 && m_cp_start_shift > 0) {
            effective_parent = static_cast<int32_t>(slot) + m_cp_start_shift;
        }
        if (cp.worldspace) {
            // Author opted out of the parent chain by marking the CP as world-
            // space — its `offset` is already in scene coordinates, so chain
            // resolution and follow-parent-particle would both shift it
            // incorrectly.  Use the local offset verbatim.
            cp.resolved      = cp.offset;
            resolved_is_null = cp.is_null_offset;
        } else if (cp.follow_parent_particle && has_bound_particle) {
            cp.resolved     = bound_pos;
        } else if (effective_parent > 0 && m_parent_subsystem != nullptr) {
            auto parent_cps = m_parent_subsystem->Controlpoints();
            // Clamp defensively; effective_parent may exceed the parent table when
            // the author bound a high slot or when the shift carries us past the end.
            auto idx = static_cast<size_t>(
                std::clamp(effective_parent, 0, (int32_t)parent_cps.size() - 1));
            cp.resolved     = parent_cps[idx].resolved;
            resolved_is_null =
                parent_cps[idx].is_null_offset || parent_cps[idx].is_null_resolved;
        } else {
            cp.resolved     = cp.offset;
            resolved_is_null = cp.is_null_offset;
        }
        if (resolved_is_null && has_bound_particle) {
            cp.resolved = bound_pos;
        }
        cp.is_null_resolved = resolved_is_null;

        // Per-frame velocity = (resolved - prev_resolved) / dt.  Skip the first
        // frame (no history) and any frame where dt is non-positive — both
        // cases leave velocity at zero, which is the correct default.
        const double dt = m_sys.scene.frameTime;
        if (cp.has_prev_resolved && dt > 1e-9) {
            cp.velocity = (cp.resolved - cp.prev_resolved) / dt;
        } else {
            cp.velocity.setZero();
        }
        cp.prev_resolved     = cp.resolved;
        cp.has_prev_resolved = true;
    }
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
    // property tick based on bass amplitude.  Audio multiplier composes
    // multiplicatively when the subsystem is flagged audio-reactive
    // (audioprocessingmode != 0); otherwise it stays at 1.0.
    double rate_eff     = m_rate * m_dynamic_rate_multiplier.load() *
                          m_audio_rate_multiplier.load();
    double particleTime = frameTime * rate_eff;
    m_time += particleTime;

    // STATIC subsystems auto-create one unbounded instance UNLESS they're nested under
    // an EVENT_FOLLOW parent.  In that special case (NieR thunderbolt_beam_child under
    // thunderbolt_child_spawner), the parent's spawn_inst loop hands out one instance
    // per parent particle so each sub-beam tracks its own spark via bounded_data.pos.
    //
    // A STATIC child nested under a STATIC parent (NieR thunderbolt_glow under
    // thunderbolt) is NOT per-parent-particle — the author intended a single
    // instantaneous-burst subsystem that fires once in parent's coord space.  See
    // memory/cp-parent-chain.md for the audit.
    const bool parent_is_event_follow =
        (m_parent_subsystem != nullptr &&
         m_parent_subsystem->Type() == SpawnType::EVENT_FOLLOW);
    if (m_spawn_type == SpawnType::STATIC && ! parent_is_event_follow) {
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

        // Nested STATIC under EVENT_FOLLOW (NieR thunderbolt_beam_child under
        // thunderbolt_child_spawner) gets per-parent-particle instancing via the parent's
        // spawn_inst loop, so it needs parent-particle-lifetime death semantics too —
        // otherwise only the first `maxcount_instance` parent particles ever get an instance
        // and subsequent ones are silently dropped.
        //
        // A STATIC child under a STATIC parent (NieR thunderbolt_glow under thunderbolt)
        // is a single auto-created instance and does NOT bind to a parent particle — its
        // lifetime is the scene's, not the parent particle's.
        bool type_has_death =
            m_spawn_type == SpawnType::EVENT_SPAWN || m_spawn_type == SpawnType::EVENT_FOLLOW ||
            (m_spawn_type == SpawnType::STATIC && parent_is_event_follow);

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

        // Clear on death is ONLY safe for EVENT_FOLLOW — those children conceptually
        // vanish with their parent (trails, attached particles, etc.).  For nested
        // STATIC children (thunderbolt_glow sprites, thunderbolt_beam_child sub-bolts
        // under child_spawner), clearing mid-animation cuts particles off at whatever
        // alpha they were at and produces a visible "pop" on every parent-particle
        // death.  Instead, setting m_is_death is enough: the subsequent
        // `if (! inst->IsDeath())` gate (a few lines down) blocks new emissions, and
        // existing particles age out naturally through their alphafade/alphachange
        // operators.  Once all live particles die off, the instance becomes
        // NoLiveParticle + IsDeath and QueryNewInstance recycles it for the next
        // parent particle's binding.
        if (inst->IsDeath() && m_spawn_type == SpawnType::EVENT_FOLLOW) {
            inst->ParticlesVec().clear();
        }

        // Resolve CP offsets now — bounded_data.pos is fresh for this instance, and the
        // resolver needs to run BEFORE emitters (which execute initializers like
        // mapsequencebetweencontrolpoints that read CPs) AND before operators (line
        // ~290 below).  Parent subsystem's Emitt has already run (top-down recursion in
        // ParticleSystem::Emitt), so parent CPs are already resolved this frame.
        ResolveControlpointsForInstance(inst.get());

        // Pre-emit particle count so we can detect newly-spawned particles by
        // counting how many got appended this frame.  Used by the
        // WEKDE_DEBUG_PARTICLE first-emit log to peek at init values.
        const usize pre_emit_count = inst->ParticlesVec().size();

        if (! inst->IsDeath()) {
            // Plumb the currently-spawning instance through a thread-local cell so
            // initializers (notably `inheritinitialvaluefromevent`) can resolve the
            // parent-event particle without a signature change on ParticleInitOp.
            particle_spawn_context::SetSpawnInstance(inst.get());
            for (auto& emittOp : m_emiters) {
                emittOp(inst->ParticlesVec(), m_initializers, m_maxcount, particleTime);
            }
            particle_spawn_context::SetSpawnInstance(nullptr);
        }

        // First-emit diagnostic: when WEKDE_DEBUG_PARTICLE is set, log the init
        // values of the very first particle emitted per subsystem name.  Reveals
        // whether `lifetimerandom` actually applied (init.lifetime should match
        // the authored range, not the C++ default 1.0) and whether init.alpha/
        // init.size landed where authoring intended.  One log per name across
        // the lifetime of the process — the `static thread_local` flag is
        // checked-and-set by name, so re-arming the subsystem (Reset → fresh
        // Emitt) won't repeat the line.
        static const bool particle_debug_first_emit = [] {
            const char* v = std::getenv("WEKDE_DEBUG_PARTICLE");
            return v && v[0] != '\0' && v[0] != '0';
        }();
        if (particle_debug_first_emit && ! m_debug_name.empty() &&
            inst->ParticlesVec().size() > pre_emit_count) {
            static thread_local std::map<std::string, bool, std::less<>> first_emit_logged;
            auto& seen = first_emit_logged[m_debug_name];
            if (! seen) {
                seen = true;
                const Particle& p = inst->ParticlesVec()[pre_emit_count];
                LOG_INFO("particle-first-emit: '%s' init.lifetime=%.4g init.alpha=%.3f "
                         "init.size=%.2f init.color=(%.2f,%.2f,%.2f) "
                         "lifetime=%.4g alpha=%.3f spawn_type=%d",
                         m_debug_name.c_str(),
                         p.init.lifetime, p.init.alpha,
                         p.init.size,
                         p.init.color.x(), p.init.color.y(), p.init.color.z(),
                         p.lifetime, p.alpha,
                         (int)m_spawn_type);
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
            .instance      = inst.get(),
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
                // new spawn — extend to STATIC children ONLY when we are EVENT_FOLLOW,
                // so sub-particles nested under an event_follow parent (NieR
                // thunderbolt_beam_child under thunderbolt_child_spawner) get an
                // instance per parent particle, bounded to it for CP resolution.
                //
                // STATIC children of a non-event parent (NieR thunderbolt_glow under
                // thunderbolt) must NOT be per-parent-particle — they are a single
                // shared instance auto-created above.  Spawning one per bolt particle
                // stacks 64×4 glow bursts at origin, overpowering the intended
                // "subtle flash when bolt starts" effect.
                for (auto& child : m_children) {
                    auto ct = child->Type();
                    if (ct == SpawnType::EVENT_FOLLOW || ct == SpawnType::EVENT_SPAWN)
                        spawn_inst(*inst, *child, i);
                    else if (ct == SpawnType::STATIC &&
                             m_spawn_type == SpawnType::EVENT_FOLLOW)
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

    // Rate-limited snapshot: live instances, live particle total, alpha + life
    // distribution, and the bounded parent pos of up to 3 instances.  Logs
    // every ~120 frames per subsystem (≈2s at 60fps) only for named subsystems.
    // Gated on WEKDE_DEBUG_PARTICLE env var so production journal stays quiet.
    //
    // The alpha + life-left stats answer "are particles disappearing too
    // quickly?" diagnostically: a healthy population has avg_alpha near
    // peak-of-curve and life_left buckets evenly distributed across the four
    // quartiles.  A population that fades too fast shows max_alpha < 1.0 and
    // most particles in the 75-100% life-left bucket (newly spawned, briefly
    // visible, then immediately culled).  A population whose particles die
    // before lifetime expiry shows life_left buckets all at the high end and
    // a low live count vs maxcount.
    static const bool particle_debug_enabled = [] {
        const char* v = std::getenv("WEKDE_DEBUG_PARTICLE");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (particle_debug_enabled && ! m_debug_name.empty()) {
        static thread_local std::map<std::string, u32, std::less<>> frame_counts;
        u32& fc = frame_counts[m_debug_name];
        if ((fc++ % 120) == 0) {
            usize live_inst = 0, live_p = 0;
            std::array<Eigen::Vector3f, 3> live_samples { Eigen::Vector3f::Zero(),
                                                          Eigen::Vector3f::Zero(),
                                                          Eigen::Vector3f::Zero() };
            usize sample_n = 0;
            // Alpha + life statistics across the live population.  Life-left
            // is `lifetime / init.lifetime ∈ [0,1]`: 1.0 means freshly emitted,
            // 0.0 means at-or-past death.  Buckets count particles in each
            // quartile so we can see the distribution at a glance.
            float alpha_sum = 0.0f, alpha_min = 1e9f, alpha_max = -1.0f;
            float life_sum = 0.0f, life_min = 1e9f, life_max = -1.0f;
            float init_life_min = 1e9f, init_life_max = -1.0f;
            std::array<usize, 4> life_buckets { 0, 0, 0, 0 };
            for (auto& inst : m_instances) {
                if (! inst) continue;
                bool any_live = false;
                for (auto& p : inst->ParticlesVec()) {
                    if (! ParticleModify::LifetimeOk(p)) continue;
                    live_p++;
                    any_live = true;
                    const float L = p.init.lifetime;
                    const float life_left = (L > 1e-6f) ? (p.lifetime / L) : 0.0f;
                    alpha_sum += p.alpha;
                    if (p.alpha < alpha_min) alpha_min = p.alpha;
                    if (p.alpha > alpha_max) alpha_max = p.alpha;
                    life_sum += life_left;
                    if (life_left < life_min) life_min = life_left;
                    if (life_left > life_max) life_max = life_left;
                    if (L < init_life_min) init_life_min = L;
                    if (L > init_life_max) init_life_max = L;
                    // 0..0.25 → bucket 0, 0.25..0.5 → 1, 0.5..0.75 → 2, 0.75..1.0 → 3.
                    int b = std::clamp((int)(life_left * 4.0f), 0, 3);
                    life_buckets[b]++;
                }
                if (any_live) {
                    if (sample_n < 3) live_samples[sample_n++] = inst->GetBoundedData().pos;
                    live_inst++;
                }
            }
            const float alpha_avg = live_p ? alpha_sum / (float)live_p : 0.0f;
            const float life_avg  = live_p ? life_sum  / (float)live_p : 0.0f;
            if (live_p == 0) {
                alpha_min = alpha_max = life_min = life_max = 0.0f;
                init_life_min = init_life_max = 0.0f;
            }
            LOG_INFO("particle-state: '%s' inst=%zu/%zu liveP=%zu "
                     "alpha[avg/min/max]=%.2f/%.2f/%.2f "
                     "life_left[avg/min/max]=%.2f/%.2f/%.2f "
                     "init_life=%.2f..%.2f "
                     "buckets[0-25,25-50,50-75,75-100]=%zu/%zu/%zu/%zu "
                     "livePos[0]=(%.1f,%.1f) livePos[1]=(%.1f,%.1f) livePos[2]=(%.1f,%.1f)",
                     m_debug_name.c_str(),
                     live_inst,
                     m_instances.size(),
                     live_p,
                     alpha_avg, alpha_min, alpha_max,
                     life_avg, life_min, life_max,
                     init_life_min, init_life_max,
                     life_buckets[0], life_buckets[1], life_buckets[2], life_buckets[3],
                     live_samples[0].x(), live_samples[0].y(),
                     live_samples[1].x(), live_samples[1].y(),
                     live_samples[2].x(), live_samples[2].y());
        }
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

// Recursive walk to find the largest starttime across the entire subsystem
// tree.  Top-level subsystems live in `subsystems`; each may have children
// with their own starttime.
static double MaxStartTimeRec(const ParticleSubSystem& s) {
    double m = (double)s.StartTime();
    for (const auto& c : s.Children()) {
        if (c) m = std::max(m, MaxStartTimeRec(*c));
    }
    return m;
}

double ParticleSystem::MaxStartTime() const {
    double m = 0.0;
    for (const auto& s : subsystems) {
        if (s) m = std::max(m, MaxStartTimeRec(*s));
    }
    return m;
}

static void ClearStartTimeRec(ParticleSubSystem& s) {
    s.ClearStartTime();
    for (const auto& c : s.Children()) {
        if (c) ClearStartTimeRec(*c);
    }
}

void ParticleSystem::PreSimulate(double dt) {
    double target = MaxStartTime();
    if (target <= 0.0 || dt <= 0.0) return;

    // Save and restore the scene clock — callers set this up just once at
    // scene load; we don't want to leak pre-sim time into the first real
    // frame's shader `g_Time` uniform or animation timers.
    double saved_elapsed    = scene.elapsingTime;
    double saved_frameTime  = scene.frameTime;

    scene.elapsingTime = 0.0;
    scene.frameTime    = dt;

    // Each subsystem's own `m_starttime` gate inside Emitt() naturally
    // staggers emission: if maxT=200 and a subsystem's starttime=50, the
    // first 50s of ticks no-op, then it emits for 150s.  That matches WE's
    // "N seconds of simulation before the first frame" semantic.
    int tick_count = 0;
    while (scene.elapsingTime < target) {
        Emitt();
        scene.elapsingTime += dt;
        ++tick_count;
    }

    LOG_INFO("ParticleSystem::PreSimulate: advanced %d ticks (%.1fs) at dt=%.4fs",
             tick_count,
             target,
             dt);

    // Now every subsystem has been run forward; clear their starttime
    // gates so real rendering doesn't re-block emission from scratch.
    for (auto& s : subsystems) {
        if (s) ClearStartTimeRec(*s);
    }

    scene.elapsingTime = saved_elapsed;
    scene.frameTime    = saved_frameTime;
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
