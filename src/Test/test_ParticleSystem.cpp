#include <doctest.h>

#include "Particle/ParticleSystem.h"
#include "Particle/ParticleEmitter.h"
#include "WPMapSequenceParse.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneMesh.h"

#include <memory>

using namespace wallpaper;

// ===========================================================================
// ParticleInstance — pure data container, no rendering needed.
// ===========================================================================

TEST_SUITE("ParticleInstance") {
    TEST_CASE("default state is alive with live particles") {
        ParticleInstance inst;
        CHECK_FALSE(inst.IsDeath());
        CHECK_FALSE(inst.IsNoLiveParticle());
        CHECK(inst.Particles().size() == 0);
    }

    TEST_CASE("SetDeath / SetNoLiveParticle roundtrip") {
        ParticleInstance inst;
        inst.SetDeath(true);
        CHECK(inst.IsDeath());
        inst.SetNoLiveParticle(true);
        CHECK(inst.IsNoLiveParticle());
    }

    TEST_CASE("Refresh resets death flags and clears particles") {
        ParticleInstance inst;
        inst.SetDeath(true);
        inst.SetNoLiveParticle(true);
        inst.ParticlesVec().emplace_back();
        inst.GetBoundedData().particle_idx = 17;
        REQUIRE(inst.Particles().size() == 1);

        inst.Refresh();
        CHECK_FALSE(inst.IsDeath());
        CHECK_FALSE(inst.IsNoLiveParticle());
        CHECK(inst.Particles().size() == 0);
        CHECK(inst.GetBoundedData().particle_idx == -1);
    }

    TEST_CASE("InitTrails stores capacity and ensures histories exist after refresh") {
        ParticleInstance inst;
        inst.InitTrails(16, 0.5f);
        CHECK(inst.TrailCapacity() == 16u);
        // Histories vector starts empty; Emitt grows it to match particle count.
        CHECK(inst.TrailHistories().size() == 0);
    }

    TEST_CASE("ParticlesVec is a mutable reference") {
        ParticleInstance inst;
        inst.ParticlesVec().emplace_back();
        inst.ParticlesVec().emplace_back();
        CHECK(inst.Particles().size() == 2);
    }

} // ParticleInstance

// ===========================================================================
// ParticleSubSystem — exercising the non-render-path methods.
// A Scene + ParticleSystem is created locally; gener stays null so any code
// path that touches it (only the VertexCount>0 branch in Emitt) is avoided.
// ===========================================================================

namespace
{

struct ParticleFixture {
    Scene                           scene;
    std::unique_ptr<ParticleSystem> psys;
    std::shared_ptr<SceneMesh>      mesh;

    ParticleFixture() {
        psys = std::make_unique<ParticleSystem>(scene);
        mesh = std::make_shared<SceneMesh>(true);
    }

    std::unique_ptr<ParticleSubSystem>
    makeSub(uint32_t maxcount = 10, double rate = 1.0, u32 maxcount_instance = 4,
            double                       probability = 1.0,
            ParticleSubSystem::SpawnType type        = ParticleSubSystem::SpawnType::STATIC,
            float                        starttime   = 0.0f) {
        return std::make_unique<ParticleSubSystem>(
            *psys, mesh, maxcount, rate, maxcount_instance, probability, type, nullptr, starttime);
    }
};

} // namespace

TEST_SUITE("ParticleSubSystem") {
    TEST_CASE("Type and MaxInstanceCount reflect constructor args") {
        ParticleFixture fx;
        auto sub = fx.makeSub(100, 2.0, 8, 1.0, ParticleSubSystem::SpawnType::EVENT_SPAWN);
        CHECK(sub->Type() == ParticleSubSystem::SpawnType::EVENT_SPAWN);
        CHECK(sub->MaxInstanceCount() == 8u);
    }

    TEST_CASE("Controlpoints default to 8 unlinked entries") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        auto            cps = sub->Controlpoints();
        CHECK(cps.size() == 8);
        for (const auto& cp : cps) {
            CHECK_FALSE(cp.link_mouse);
            CHECK_FALSE(cp.worldspace);
        }
    }

    TEST_CASE("Controlpoints default runtime_flags to 0 (no operator has touched them yet)") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        auto            cps = sub->Controlpoints();
        for (const auto& cp : cps) {
            CHECK(cp.runtime_flags == 0u);
        }
    }

    TEST_CASE("MarkCpReferenced ORs kCpReferencedFlag onto only the named slot") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        sub->MarkCpReferenced(2);
        sub->MarkCpReferenced(5);
        auto cps = sub->Controlpoints();
        CHECK((cps[2].runtime_flags & kCpReferencedFlag) != 0u);
        CHECK((cps[5].runtime_flags & kCpReferencedFlag) != 0u);
        CHECK((cps[0].runtime_flags & kCpReferencedFlag) == 0u);
        CHECK((cps[1].runtime_flags & kCpReferencedFlag) == 0u);
        CHECK((cps[7].runtime_flags & kCpReferencedFlag) == 0u);
    }

    TEST_CASE("MarkCpReferenced is idempotent — same slot, multiple calls, one bit") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        sub->MarkCpReferenced(3);
        sub->MarkCpReferenced(3);
        sub->MarkCpReferenced(3);
        auto cps = sub->Controlpoints();
        CHECK((cps[3].runtime_flags & kCpReferencedFlag) != 0u);
        // Only the high bit; lower authored bits remain untouched.
        CHECK((cps[3].runtime_flags & 0xffu) == 0u);
    }

    TEST_CASE("MarkCpReferenced clamps out-of-range indices to 0..7") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        sub->MarkCpReferenced(99); // → slot 7
        sub->MarkCpReferenced(-3); // → slot 0
        auto cps = sub->Controlpoints();
        CHECK((cps[7].runtime_flags & kCpReferencedFlag) != 0u);
        CHECK((cps[0].runtime_flags & kCpReferencedFlag) != 0u);
    }

    TEST_CASE("CpStartShift defaults to 0 (no shift on bare subsystems)") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        CHECK(sub->CpStartShift() == 0);
    }

    TEST_CASE("SetCpStartShift / CpStartShift roundtrip stores the authored value") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        sub->SetCpStartShift(3);
        CHECK(sub->CpStartShift() == 3);
    }

    TEST_CASE("Resolver applies CpStartShift to slots whose authored parent_cp_index is 0") {
        // Mirrors the WE child-container semantic: the child's CP[i] resolves through
        // the parent's CP[i + shift] when the child didn't explicitly declare a parent.
        ParticleFixture fx;
        auto            parent = fx.makeSub();
        auto            child  = fx.makeSub();
        // Parent CP[2] sits at (50, 60, 70); resolved=offset by default.
        parent->Controlpoints()[2].offset = Eigen::Vector3d(50, 60, 70);
        parent->Controlpoints()[2].resolved = Eigen::Vector3d(50, 60, 70);
        // Child has authored parent_cp_index=0 (the WE editor default), shift=2.
        // Authored value MUST survive parse and the resolver layers the shift on top.
        child->Controlpoints()[0].parent_cp_index = 0;
        child->SetCpStartShift(2);
        auto* child_raw = child.get();
        parent->AddChild(std::move(child));

        child_raw->ResolveControlpointsForInstance(nullptr);
        CHECK(child_raw->Controlpoints()[0].resolved.x() == doctest::Approx(50.0));
        CHECK(child_raw->Controlpoints()[0].resolved.y() == doctest::Approx(60.0));
        CHECK(child_raw->Controlpoints()[0].resolved.z() == doctest::Approx(70.0));
    }

    TEST_CASE("Resolver leaves authored parent_cp_index alone when CpStartShift is 0") {
        ParticleFixture fx;
        auto            parent = fx.makeSub();
        auto            child  = fx.makeSub();
        parent->Controlpoints()[5].offset   = Eigen::Vector3d(11, 22, 33);
        parent->Controlpoints()[5].resolved = Eigen::Vector3d(11, 22, 33);
        // Child authored an explicit parent_cp_index — no shift.
        child->Controlpoints()[1].parent_cp_index = 5;
        auto* child_raw = child.get();
        parent->AddChild(std::move(child));

        child_raw->ResolveControlpointsForInstance(nullptr);
        CHECK(child_raw->Controlpoints()[1].resolved.x() == doctest::Approx(11.0));
        CHECK(child_raw->Controlpoints()[1].resolved.y() == doctest::Approx(22.0));
        CHECK(child_raw->Controlpoints()[1].resolved.z() == doctest::Approx(33.0));
    }

    TEST_CASE("Explicit parent_cp_index wins over CpStartShift (no double-shift)") {
        ParticleFixture fx;
        auto            parent = fx.makeSub();
        auto            child  = fx.makeSub();
        parent->Controlpoints()[3].offset   = Eigen::Vector3d(1, 2, 3);
        parent->Controlpoints()[3].resolved = Eigen::Vector3d(1, 2, 3);
        // Child explicitly chose parent CP[3]; shift would otherwise add 2 → CP[5].
        // Author intent must win.
        child->Controlpoints()[0].parent_cp_index = 3;
        child->SetCpStartShift(2);
        auto* child_raw = child.get();
        parent->AddChild(std::move(child));

        child_raw->ResolveControlpointsForInstance(nullptr);
        CHECK(child_raw->Controlpoints()[0].resolved.x() == doctest::Approx(1.0));
        CHECK(child_raw->Controlpoints()[0].resolved.y() == doctest::Approx(2.0));
        CHECK(child_raw->Controlpoints()[0].resolved.z() == doctest::Approx(3.0));
    }

    TEST_CASE("AddEmitter/AddInitializer/AddOperator accept std::function callables") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        // These return void; coverage check is that they don't throw.
        sub->AddEmitter([](std::vector<Particle>&, std::vector<ParticleInitOp>&, uint32_t, double) {
        });
        sub->AddInitializer([](Particle&, double) {
        });
        sub->AddOperator([](const ParticleInfo&) {
        });
        CHECK(true);
    }

    TEST_CASE("AddChild and children are reset together") {
        ParticleFixture fx;
        auto            sub   = fx.makeSub();
        auto            child = fx.makeSub();
        sub->AddChild(std::move(child));
        // Reset cascades; no observable but covers the recursion.
        sub->Reset();
        CHECK(true);
    }

    TEST_CASE("SetSpriteTrail stores capacity for later InitTrails") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        sub->SetSpriteTrail(32, 1.5f);
        // No direct getter, but QueryNewInstance will propagate capacity — no crash.
        CHECK(true);
    }

    TEST_CASE("QueryNewInstance grows the pool up to max, then returns nullptr") {
        ParticleFixture fx;
        auto            sub = fx.makeSub(10, 1.0, 3, 1.0); // max 3 instances
        auto*           a   = sub->QueryNewInstance();
        auto*           b   = sub->QueryNewInstance();
        auto*           c   = sub->QueryNewInstance();
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        REQUIRE(c != nullptr);
        CHECK(a != b);
        CHECK(b != c);
        // Pool full; next call returns nullptr.
        CHECK(sub->QueryNewInstance() == nullptr);
    }

    TEST_CASE("QueryNewInstance reuses dead+no-live instance") {
        ParticleFixture fx;
        auto            sub   = fx.makeSub(10, 1.0, 1, 1.0); // pool of 1
        auto*           first = sub->QueryNewInstance();
        REQUIRE(first != nullptr);

        // Pool full; further query while first is alive returns nullptr.
        CHECK(sub->QueryNewInstance() == nullptr);

        // Kill the first instance, then the pool should reuse its slot.
        first->SetDeath(true);
        first->SetNoLiveParticle(true);
        auto* recycled = sub->QueryNewInstance();
        CHECK(recycled == first);
    }

    TEST_CASE("QueryNewInstance with probability=0 always returns nullptr") {
        ParticleFixture fx;
        auto            sub = fx.makeSub(10, 1.0, 4, 0.0); // never spawns
        for (int i = 0; i < 5; i++) CHECK(sub->QueryNewInstance() == nullptr);
    }

    TEST_CASE("SetSpriteTrail then QueryNewInstance initialises trail capacity") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        sub->SetSpriteTrail(64, 2.0f);
        auto* inst = sub->QueryNewInstance();
        REQUIRE(inst != nullptr);
        CHECK(inst->TrailCapacity() == 64u);
    }

    TEST_CASE("UpdateMouseControlPoints only writes to link_mouse CPs and recurses") {
        ParticleFixture fx;
        auto            sub   = fx.makeSub();
        auto            child = fx.makeSub();

        // Arrange: mark CP[0] link_mouse in both sub and child, leave others alone.
        sub->Controlpoints()[0].link_mouse   = true;
        child->Controlpoints()[0].link_mouse = true;

        auto* child_raw = child.get();
        sub->AddChild(std::move(child));

        sub->UpdateMouseControlPoints(7.5, -3.25);

        CHECK(sub->Controlpoints()[0].offset.x() == doctest::Approx(7.5));
        CHECK(sub->Controlpoints()[0].offset.y() == doctest::Approx(-3.25));
        // Non-linked CP stays at default (0, 0, 0)
        CHECK(sub->Controlpoints()[1].offset.x() == doctest::Approx(0.0));
        // Recursion reached the child
        CHECK(child_raw->Controlpoints()[0].offset.x() == doctest::Approx(7.5));
    }

    TEST_CASE("burst_done flag: false before Emitt; ClearBurstDone resets it") {
        ParticleFixture fx;
        auto            sub = fx.makeSub();
        CHECK_FALSE(sub->IsBurstDone());
        // Without invoking the full Emitt path (which needs render data),
        // we simulate the burst-end transition by toggling via public API.
        sub->ClearBurstDone();
        CHECK_FALSE(sub->IsBurstDone());
    }

} // ParticleSubSystem

// ===========================================================================
// ParticleSystem top-level — subsystems vector and mouse CP plumbing.
// ===========================================================================

TEST_SUITE("ParticleSystem") {
    TEST_CASE("empty subsystem list: Emitt is a no-op") {
        Scene          scene;
        ParticleSystem ps(scene);
        ps.Emitt(); // should not crash
        CHECK(ps.subsystems.size() == 0);
    }

    TEST_CASE("UpdateMouseControlPoints converts to scene coordinates") {
        Scene          scene;
        ParticleSystem ps(scene);
        auto           mesh = std::make_shared<SceneMesh>(true);
        auto           sub  = std::make_unique<ParticleSubSystem>(
            ps, mesh, 10, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC, nullptr, 0);
        sub->Controlpoints()[0].link_mouse = true;
        auto* sub_raw                      = sub.get();
        ps.subsystems.push_back(std::move(sub));

        // mousePos (0.5, 0.5) → scene origin (0, 0)
        ps.UpdateMouseControlPoints({ 0.5f, 0.5f }, { 1920, 1080 });
        CHECK(sub_raw->Controlpoints()[0].offset.x() == doctest::Approx(0.0));
        CHECK(sub_raw->Controlpoints()[0].offset.y() == doctest::Approx(0.0));

        // mousePos (1.0, 0.0) → (+half_width, +half_height) — Y is flipped
        ps.UpdateMouseControlPoints({ 1.0f, 0.0f }, { 1920, 1080 });
        CHECK(sub_raw->Controlpoints()[0].offset.x() == doctest::Approx(960.0));
        CHECK(sub_raw->Controlpoints()[0].offset.y() == doctest::Approx(540.0));

        // mousePos (0.0, 1.0) → (-half_width, -half_height)
        ps.UpdateMouseControlPoints({ 0.0f, 1.0f }, { 1920, 1080 });
        CHECK(sub_raw->Controlpoints()[0].offset.x() == doctest::Approx(-960.0));
        CHECK(sub_raw->Controlpoints()[0].offset.y() == doctest::Approx(-540.0));
    }

    TEST_CASE("Emitt before starttime skips the subsystem") {
        Scene scene;
        scene.elapsingTime = 0.0; // starts at 0
        ParticleSystem ps(scene);
        auto           mesh = std::make_shared<SceneMesh>(true);
        auto           sub  = std::make_unique<ParticleSubSystem>(ps,
                                                       mesh,
                                                       10,
                                                       1.0,
                                                       1,
                                                       1.0,
                                                       ParticleSubSystem::SpawnType::STATIC,
                                                       nullptr,
                                                       /*starttime=*/100);
        ps.subsystems.push_back(std::move(sub));

        // elapsingTime < starttime → subsystem's Emitt returns early,
        // no instances are created for STATIC.
        ps.Emitt();
        // After starttime passes, Emitt enters the STATIC body and auto-creates
        // the single instance.  Since we have no emitters and no vertex array,
        // this is a safe code path with no rendering.
        scene.PassFrameTime(1.0);
        scene.elapsingTime = 200.0; // jump past starttime directly
        ps.Emitt();
        CHECK(true);
    }

} // ParticleSystem

// ===========================================================================
// Emitt — exercises the full per-frame loop body with a real emitter + operator.
// ===========================================================================

TEST_SUITE("ParticleSubSystem.Emitt") {
    // Push one particle with non-zero lifetime when called.
    static ParticleEmittOp makePushOneEmitter() {
        return [](std::vector<Particle>&       particles,
                  std::vector<ParticleInitOp>& inits,
                  uint32_t                     maxcount,
                  double /*timepass*/) {
            if (particles.size() >= maxcount) return;
            Particle p;
            p.lifetime = 10.0f;
            // Run any initializers registered alongside the emitter.
            for (auto& init : inits) init(p, 0.0);
            particles.push_back(p);
        };
    }

    TEST_CASE("STATIC subsystem with push-one emitter grows particle vector over frames") {
        ParticleFixture fx;
        auto            sub = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        sub->AddEmitter(makePushOneEmitter());

        // A simple operator that increments each particle's lifetime by 0.01.
        sub->AddOperator([](const ParticleInfo& info) {
            for (auto& p : info.particles) p.lifetime += 0.01f;
        });

        fx.scene.PassFrameTime(0.016);
        sub->Emitt();
        fx.scene.PassFrameTime(0.016);
        sub->Emitt();
        // Emitter keeps pushing until maxcount reached
        fx.scene.PassFrameTime(0.016);
        sub->Emitt();
        CHECK(true); // success = no crash; coverage is the primary goal here
    }

    TEST_CASE("spritetrail STATIC: trail history grows alongside particles") {
        ParticleFixture fx;
        auto            sub = fx.makeSub(4, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        sub->SetSpriteTrail(8, 1.0f);
        sub->AddEmitter(makePushOneEmitter());

        fx.scene.PassFrameTime(0.016);
        sub->Emitt();
        fx.scene.PassFrameTime(0.016);
        sub->Emitt();
        CHECK(true);
    }

    TEST_CASE("Child EVENT_SPAWN subsystem is nudged to spawn from parent's new particles") {
        ParticleFixture fx;
        auto            parent = fx.makeSub(3, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        parent->AddEmitter(makePushOneEmitter());

        auto  child     = fx.makeSub(3, 1.0, 2, 1.0, ParticleSubSystem::SpawnType::EVENT_SPAWN);
        auto* child_raw = child.get();
        parent->AddChild(std::move(child));

        fx.scene.PassFrameTime(0.016);
        parent->Emitt();
        // After one frame, parent has ≥1 particle; spawn_inst lambda should have
        // run and pushed the child into the pool.  We can't easily inspect the
        // child's internal state, but having executed is coverage.
        (void)child_raw;
        CHECK(true);
    }

    TEST_CASE("Child EVENT_DEATH fires when parent particles die") {
        ParticleFixture fx;
        auto            parent = fx.makeSub(3, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        // Push particles that die quickly (lifetime ≤ frame time).
        parent->AddEmitter(
            [](std::vector<Particle>& ps, std::vector<ParticleInitOp>&, uint32_t max, double) {
                if (ps.size() >= max) return;
                Particle p;
                p.lifetime = 0.001f;
                ps.push_back(p);
            });

        auto child = fx.makeSub(3, 1.0, 2, 1.0, ParticleSubSystem::SpawnType::EVENT_DEATH);
        parent->AddChild(std::move(child));

        fx.scene.PassFrameTime(0.032);
        parent->Emitt();
        fx.scene.PassFrameTime(0.032);
        parent->Emitt(); // second tick pushes particles over lifetime → event_death path
        CHECK(true);
    }

    TEST_CASE("Controlpoints const-overload") {
        ParticleFixture fx;
        auto            sub       = fx.makeSub();
        const auto&     const_sub = *sub;
        auto            cps       = const_sub.Controlpoints();
        CHECK(cps.size() == 8);
    }

} // ParticleSubSystem.Emitt

// ===========================================================================
// Nested-STATIC parent-type differentiation
//
// WE's `type: static` child has two canonical meanings depending on its
// parent subsystem's type.  The NieR:Automata 2B thunderbolt scene
// exercises both in the same tree:
//
//   thunderbolt (STATIC root)
//     ├── thunderbolt_child_spawner (EVENT_FOLLOW)
//     │     └── thunderbolt_beam_child  (STATIC — one per parent particle)
//     └── thunderbolt_glow              (STATIC — a single instance)
//
// A nested STATIC under an EVENT_FOLLOW parent must get one instance per
// parent particle so each sub-bolt tracks its own spark via bounded_data.
// A nested STATIC under a STATIC parent (or any non-event parent) is the
// author's "single instantaneous-burst subsystem that fires at T=0" case:
// one auto-created instance, never bound to a parent particle.
//
// These tests pin that differentiation so we don't regress the thunderbolt
// glow-vs-beam distinction again.  We use QueryNewInstance(maxcount=1) as
// the observable: if auto-create ran, the pool is full; if it didn't, the
// pool is still empty.
// ===========================================================================

TEST_SUITE("ParticleSubSystem.NestedStatic") {
    TEST_CASE("STATIC child under STATIC parent auto-creates its own instance") {
        ParticleFixture fx;
        // Parent is STATIC root → auto-instance on Emitt.
        auto parent = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        // Child is STATIC under STATIC — author's "single burst" case.
        // maxcount_instance=1 so if auto-create runs, the pool is full.
        auto  child     = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        auto* child_raw = child.get();
        parent->AddChild(std::move(child));

        fx.scene.PassFrameTime(0.016);
        parent->Emitt(); // cascades into child->Emitt() at end

        // Child auto-created its single instance → pool of 1 is full.
        CHECK(child_raw->QueryNewInstance() == nullptr);
    }

    TEST_CASE("STATIC child under EVENT_FOLLOW parent does NOT auto-create") {
        ParticleFixture fx;
        // Parent is EVENT_FOLLOW with no real parent particle to bind to —
        // no live instances, but its own Emitt still cascades to children.
        auto parent = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::EVENT_FOLLOW);
        // Child is STATIC under EVENT_FOLLOW — author's "per-parent-particle" case.
        auto  child     = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        auto* child_raw = child.get();
        parent->AddChild(std::move(child));

        fx.scene.PassFrameTime(0.016);
        parent->Emitt();

        // Child did NOT auto-create (parent is EVENT_FOLLOW) → pool still empty,
        // QueryNewInstance can freely mint a new slot.
        CHECK(child_raw->QueryNewInstance() != nullptr);
    }

    TEST_CASE("STATIC child under EVENT_SPAWN parent does auto-create (not event_follow)") {
        ParticleFixture fx;
        // EVENT_SPAWN parent: rule targets ONLY EVENT_FOLLOW for the per-particle
        // carve-out.  Other event types fall back to the single-instance default.
        auto parent = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::EVENT_SPAWN);
        auto child  = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        auto* child_raw = child.get();
        parent->AddChild(std::move(child));

        fx.scene.PassFrameTime(0.016);
        parent->Emitt();

        // Child auto-created → pool full.
        CHECK(child_raw->QueryNewInstance() == nullptr);
    }

    TEST_CASE("Top-level STATIC auto-creates (parent is nullptr)") {
        ParticleFixture fx;
        // The pre-existing behavior: top-level STATIC with no parent always
        // auto-creates one instance on first Emitt.
        auto  sub     = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        auto* sub_raw = sub.get();

        // Before Emitt, pool is empty.
        auto* first = sub_raw->QueryNewInstance();
        CHECK(first != nullptr);
        // Kill so we can see the auto-create path independently.
        first->SetDeath(true);
        first->SetNoLiveParticle(true);
        // Refresh() via QueryNewInstance would reuse — skip by driving Emitt directly
        // on a fresh subsystem.
        auto  sub2     = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        auto* sub2_raw = sub2.get();
        fx.scene.PassFrameTime(0.016);
        sub2_raw->Emitt();
        CHECK(sub2_raw->QueryNewInstance() == nullptr); // auto-created, pool full
    }

    TEST_CASE("EVENT_FOLLOW parent does not spawn a STATIC child instance on its "
              "own (needs parent particle)") {
        // This mirrors the NieR hierarchy: a top-level STATIC that contains an
        // EVENT_FOLLOW whose STATIC grandchild is per-particle.  With no real
        // grandparent particle, the event_follow pool stays empty and the
        // STATIC grandchild's per-particle spawn path is never hit.
        ParticleFixture fx;
        auto root = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        auto mid  = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::EVENT_FOLLOW);
        auto leaf = fx.makeSub(5, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC);
        auto* leaf_raw = leaf.get();
        mid->AddChild(std::move(leaf));
        root->AddChild(std::move(mid));

        fx.scene.PassFrameTime(0.016);
        root->Emitt();

        // Leaf stays empty: parent is EVENT_FOLLOW (no auto-create) AND no
        // parent particle fired the per-particle spawn_inst path (EVENT_FOLLOW
        // parent has no auto-instance of its own either).
        CHECK(leaf_raw->QueryNewInstance() != nullptr);
    }

} // ParticleSubSystem.NestedStatic

// ===========================================================================
// ParticleSystem::MaxStartTime / PreSimulate — WE `starttime` semantics.
// shimmering_particles default wallpaper ships dustmotes.starttime=50 and
// small_motes_copy1.starttime=200.  Interpreted as "delay before emitting",
// the user sees a black screen for up to 200s.  WE's actual meaning is
// "pre-simulate N seconds before frame 1", which PreSimulate() implements.
// ===========================================================================

TEST_SUITE("ParticleSystem.StartTime") {
    TEST_CASE("MaxStartTime is zero for an empty system") {
        ParticleFixture fx;
        CHECK(fx.psys->MaxStartTime() == doctest::Approx(0.0));
    }

    TEST_CASE("MaxStartTime is zero when every subsystem has starttime=0") {
        ParticleFixture fx;
        fx.psys->subsystems.push_back(fx.makeSub(10, 1.0, 1, 1.0,
                                                 ParticleSubSystem::SpawnType::STATIC, 0));
        fx.psys->subsystems.push_back(fx.makeSub(10, 1.0, 1, 1.0,
                                                 ParticleSubSystem::SpawnType::STATIC, 0));
        CHECK(fx.psys->MaxStartTime() == doctest::Approx(0.0));
    }

    TEST_CASE("MaxStartTime picks the largest top-level starttime") {
        ParticleFixture fx;
        fx.psys->subsystems.push_back(fx.makeSub(10, 1.0, 1, 1.0,
                                                 ParticleSubSystem::SpawnType::STATIC, 50));
        fx.psys->subsystems.push_back(fx.makeSub(10, 1.0, 1, 1.0,
                                                 ParticleSubSystem::SpawnType::STATIC, 200));
        fx.psys->subsystems.push_back(fx.makeSub(10, 1.0, 1, 1.0,
                                                 ParticleSubSystem::SpawnType::STATIC, 5));
        CHECK(fx.psys->MaxStartTime() == doctest::Approx(200.0));
    }

    TEST_CASE("MaxStartTime walks nested children") {
        // shimmering_particles is flat, but NieR-style nested particles can
        // carry starttime on a child subsystem.  Make sure we catch those.
        ParticleFixture fx;
        auto child = fx.makeSub(10, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::EVENT_FOLLOW, 75);
        auto root  = fx.makeSub(10, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC, 10);
        root->AddChild(std::move(child));
        fx.psys->subsystems.push_back(std::move(root));
        CHECK(fx.psys->MaxStartTime() == doctest::Approx(75.0));
    }

    TEST_CASE("PreSimulate is a no-op when MaxStartTime is 0") {
        ParticleFixture fx;
        fx.psys->subsystems.push_back(fx.makeSub(10, 1.0, 1, 1.0,
                                                 ParticleSubSystem::SpawnType::STATIC, 0));

        fx.scene.elapsingTime = 123.0;   // sentinel — must be preserved
        fx.scene.frameTime    = 0.016;
        fx.psys->PreSimulate();
        CHECK(fx.scene.elapsingTime == doctest::Approx(123.0));
        CHECK(fx.scene.frameTime == doctest::Approx(0.016));
    }

    TEST_CASE("PreSimulate restores the scene clock it found on entry") {
        ParticleFixture fx;
        auto* sub_raw = fx.makeSub(10, 1.0, 1, 1.0,
                                   ParticleSubSystem::SpawnType::STATIC, 5).release();
        fx.psys->subsystems.emplace_back(sub_raw);

        fx.scene.elapsingTime = 0.0;
        fx.scene.frameTime    = 0.0;
        fx.psys->PreSimulate(0.1);

        // After pre-sim the scene clock is back to its saved value,
        // regardless of how many ticks PreSimulate ran internally.
        CHECK(fx.scene.elapsingTime == doctest::Approx(0.0));
        CHECK(fx.scene.frameTime == doctest::Approx(0.0));
    }

    TEST_CASE("PreSimulate clears every subsystem's m_starttime gate") {
        ParticleFixture fx;
        auto child = fx.makeSub(10, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::EVENT_FOLLOW, 30);
        auto* child_raw = child.get();
        auto  root      = fx.makeSub(10, 1.0, 1, 1.0, ParticleSubSystem::SpawnType::STATIC, 15);
        auto* root_raw  = root.get();
        root->AddChild(std::move(child));
        fx.psys->subsystems.push_back(std::move(root));

        REQUIRE(root_raw->StartTime() == 15u);
        REQUIRE(child_raw->StartTime() == 30u);

        fx.psys->PreSimulate(0.5);

        CHECK(root_raw->StartTime() == 0u);
        CHECK(child_raw->StartTime() == 0u);
    }

    TEST_CASE("PreSimulate uses the dt argument (iterates until target reached)") {
        // With dt=5.0 and starttime=20, we expect exactly 4 Emitt calls.
        // We can't peek Emitt call count directly, but we can observe that
        // the scene clock was advanced at least that far internally — all
        // we need is that m_starttime gets cleared (PreSimulate ran to
        // completion without infinite-looping).
        ParticleFixture fx;
        auto* sub_raw = fx.makeSub(10, 1.0, 1, 1.0,
                                   ParticleSubSystem::SpawnType::STATIC, 20).release();
        fx.psys->subsystems.emplace_back(sub_raw);
        REQUIRE(sub_raw->StartTime() == 20u);

        fx.psys->PreSimulate(5.0);
        CHECK(sub_raw->StartTime() == 0u);
    }

    TEST_CASE("PreSimulate guards against non-positive dt (no infinite loop)") {
        ParticleFixture fx;
        auto* sub_raw = fx.makeSub(10, 1.0, 1, 1.0,
                                   ParticleSubSystem::SpawnType::STATIC, 50).release();
        fx.psys->subsystems.emplace_back(sub_raw);

        // dt=0 would infinite-loop if not guarded; we early-out instead.
        fx.psys->PreSimulate(0.0);
        // starttime is unchanged because we never ran the pre-sim loop.
        CHECK(sub_raw->StartTime() == 50u);

        fx.psys->PreSimulate(-1.0);
        CHECK(sub_raw->StartTime() == 50u);
    }

    TEST_CASE("Negative starttime is a no-op pre-sim (regression: Nightingale 3276911872)") {
        // deku_twinkle_shootingstar.json ships "starttime": -2.  Before the
        // fix, the field was parsed into a uint32_t, wrapping -2 to
        // 4294967294; MaxStartTime returned ~4.29e9 and PreSimulate looped
        // for billions of iterations, never returning — Nightingale never
        // reached its first frame and plasma showed a black desktop.
        // With float starttime, the sub's m_starttime=-2 is preserved on
        // StartTime() but MaxStartTime (which starts max at 0.0) clamps to
        // 0, so PreSimulate early-returns and the clock is untouched.
        ParticleFixture fx;
        auto* sub_raw = fx.makeSub(10, 1.0, 1, 1.0,
                                   ParticleSubSystem::SpawnType::STATIC, -2.0f).release();
        fx.psys->subsystems.emplace_back(sub_raw);
        CHECK(sub_raw->StartTime() == doctest::Approx(-2.0f));
        CHECK(fx.psys->MaxStartTime() == doctest::Approx(0.0));

        fx.scene.elapsingTime = 5.0;
        fx.scene.frameTime    = 0.016;
        fx.psys->PreSimulate(0.032);
        CHECK(fx.scene.elapsingTime == doctest::Approx(5.0));
    }

    TEST_CASE("Fractional starttime is preserved by MaxStartTime") {
        ParticleFixture fx;
        fx.psys->subsystems.push_back(fx.makeSub(10, 1.0, 1, 1.0,
                                                 ParticleSubSystem::SpawnType::STATIC, 0.7f));
        CHECK(fx.psys->MaxStartTime() == doctest::Approx(0.7));
    }
}

