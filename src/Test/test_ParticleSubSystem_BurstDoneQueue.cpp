#include <doctest.h>

#include "Particle/ParticleSystem.h"
#include "Scene/Scene.h"
#include "Scene/SceneMesh.h"

#include <memory>
#include <vector>

using namespace wallpaper;

// Edge-trigger producer-pushed queue for burst-done particle nodes.
//
// Producer (ParticleSystem::Emitt): clears m_burst_done_this_tick at the top
// of each tick, walks subsystems, and pushes the NodeId() of any sub whose
// m_burst_done is true but m_burst_done_acked is false (the false→true edge).
// After the push the producer acks the sub so subsequent ticks treat the
// level as "already published".
//
// Consumer (SceneWallpaper draw loop): drains BurstDoneThisTick(), auto-hides
// the matching node and calls ClearBurstDone() — which resets BOTH the level
// flag and the ack so a re-armed burst publishes again on the next edge.
//
// Pattern parity: the sprite-snapshot gate is the sticky-flag producer-skip
// variant; this is the edge-trigger producer-pushed variant.  Both mirror the
// WorldCacheGate.h consumer-gate intent — avoid per-frame work when no state
// change has occurred.

namespace
{

// Mirror of test_ParticleSystem.cpp's ParticleFixture (anonymous-namespace
// helper for constructing ParticleSubSystem instances without dragging in
// the renderer).
struct BurstQueueFixture {
    Scene                           scene;
    std::unique_ptr<ParticleSystem> psys;
    std::shared_ptr<SceneMesh>      mesh;

    BurstQueueFixture() {
        psys = std::make_unique<ParticleSystem>(scene);
        mesh = std::make_shared<SceneMesh>(true);
    }

    std::unique_ptr<ParticleSubSystem> makeSub() {
        return std::make_unique<ParticleSubSystem>(*psys,
                                                   mesh,
                                                   /*maxcount=*/10,
                                                   /*rate=*/1.0,
                                                   /*maxcount_instance=*/4,
                                                   /*probability=*/1.0,
                                                   ParticleSubSystem::SpawnType::STATIC,
                                                   nullptr,
                                                   /*starttime=*/0.0f);
    }
};

} // namespace

TEST_SUITE("SceneWallpaper burst-done producer queue") {
    TEST_CASE("fresh subsystem reports no ack and exposes its node id") {
        BurstQueueFixture fx;
        auto              sub = fx.makeSub();
        CHECK(sub->WasBurstDoneAcked() == false);
        CHECK(sub->NodeId() == -1); // unset default
        sub->SetNodeId(42);
        CHECK(sub->NodeId() == 42);
    }

    TEST_CASE("AckBurstDone sets sticky ack; ClearBurstDone resets both flags") {
        BurstQueueFixture fx;
        auto              sub = fx.makeSub();
        sub->AckBurstDone();
        CHECK(sub->WasBurstDoneAcked() == true);
        // Sticky across repeated reads — no auto-reset.
        CHECK(sub->WasBurstDoneAcked() == true);
        sub->ClearBurstDone();
        CHECK(sub->WasBurstDoneAcked() == false);
        CHECK(sub->IsBurstDone() == false);
    }

    TEST_CASE("ParticleSystem::Emitt drains nothing when no sub is burst-done") {
        // Spec: when no burst has completed, the per-frame walk MUST NOT find
        // any work — BurstDoneThisTick() returns an empty span.
        BurstQueueFixture fx;
        // Populate N=100 STATIC subsystems with no particles, no emitters —
        // Emitt() runs to completion without ever flipping m_burst_done.
        for (int i = 0; i < 100; ++i) {
            auto sub = fx.makeSub();
            sub->SetNodeId(1000 + i);
            fx.psys->subsystems.emplace_back(std::move(sub));
        }
        fx.psys->Emitt();
        CHECK(fx.psys->BurstDoneThisTick().empty());
    }

    TEST_CASE("ParticleSystem::Emitt detects edge and publishes node ids") {
        // The producer's contract: walk subs, push the NodeId of each one
        // whose IsBurstDone() returned true and WasBurstDoneAcked() was
        // false at the edge.  Drive the edge directly via a test-only helper
        // (TEST_MarkBurstDone) — the production path runs through Emitt's
        // any_alive_since_reset transition which needs a real emitter loop.
        BurstQueueFixture fx;
        auto              sub_a = fx.makeSub();
        auto              sub_b = fx.makeSub();
        auto              sub_c = fx.makeSub();
        sub_a->SetNodeId(7);
        sub_b->SetNodeId(11);
        sub_c->SetNodeId(13);
        sub_a->TEST_MarkBurstDone();
        // sub_b stays not-done so its id should NOT appear.
        sub_c->TEST_MarkBurstDone();

        ParticleSubSystem* raw_a = sub_a.get();
        ParticleSubSystem* raw_c = sub_c.get();
        fx.psys->subsystems.emplace_back(std::move(sub_a));
        fx.psys->subsystems.emplace_back(std::move(sub_b));
        fx.psys->subsystems.emplace_back(std::move(sub_c));

        fx.psys->Emitt();
        const auto& q = fx.psys->BurstDoneThisTick();
        REQUIRE(q.size() == 2u);
        CHECK(q[0] == 7);
        CHECK(q[1] == 13);
        // Both producer subs were ack'd.
        CHECK(raw_a->WasBurstDoneAcked() == true);
        CHECK(raw_c->WasBurstDoneAcked() == true);
    }

    TEST_CASE("edge-trigger: a sustained-true level publishes only once") {
        // Once the false→true edge has been seen, subsequent ticks must NOT
        // re-push the id.  This is the load-bearing optimization: the
        // consumer ack'd via ClearBurstDone is the only path that re-arms.
        BurstQueueFixture fx;
        auto              sub = fx.makeSub();
        sub->SetNodeId(99);
        sub->TEST_MarkBurstDone();
        ParticleSubSystem* raw = sub.get();
        fx.psys->subsystems.emplace_back(std::move(sub));

        fx.psys->Emitt();
        REQUIRE(fx.psys->BurstDoneThisTick().size() == 1u);
        CHECK(fx.psys->BurstDoneThisTick()[0] == 99);
        CHECK(raw->WasBurstDoneAcked() == true);
        CHECK(raw->IsBurstDone() == true); // level remains until ClearBurstDone

        // Second tick: still done, still ack'd → no push.
        fx.psys->Emitt();
        CHECK(fx.psys->BurstDoneThisTick().empty());
    }

    TEST_CASE("ClearBurstDone re-arms the edge for a future burst") {
        // Pool re-fire path: sub finishes a burst, drain auto-hides +
        // ClearBurstDone resets the ack; the pool then re-fires the sub
        // for a new event, m_burst_done is set true again, the producer
        // must publish on the new edge.
        BurstQueueFixture fx;
        auto              sub = fx.makeSub();
        sub->SetNodeId(55);
        sub->TEST_MarkBurstDone();
        ParticleSubSystem* raw = sub.get();
        fx.psys->subsystems.emplace_back(std::move(sub));

        fx.psys->Emitt();
        REQUIRE(fx.psys->BurstDoneThisTick().size() == 1u);
        raw->ClearBurstDone();
        CHECK(raw->WasBurstDoneAcked() == false); // edge re-armed
        CHECK(raw->IsBurstDone() == false);

        // Sub re-fires its burst.
        raw->TEST_MarkBurstDone();
        fx.psys->Emitt();
        REQUIRE(fx.psys->BurstDoneThisTick().size() == 1u);
        CHECK(fx.psys->BurstDoneThisTick()[0] == 55);
    }

    TEST_CASE("queue clears at the top of each Emitt tick") {
        // Pin the per-tick scratch semantic: yesterday's ids do NOT leak
        // into today's queue.  A sub that was ack'd before but had its
        // burst-done flag stay true should not re-appear; a fresh sub
        // whose edge fires on this tick should be the ONLY id present.
        BurstQueueFixture fx;
        auto              sub_a = fx.makeSub();
        sub_a->SetNodeId(1);
        sub_a->TEST_MarkBurstDone();
        ParticleSubSystem* raw_a = sub_a.get();
        fx.psys->subsystems.emplace_back(std::move(sub_a));

        // Tick 1 publishes sub_a.
        fx.psys->Emitt();
        REQUIRE(fx.psys->BurstDoneThisTick().size() == 1u);
        CHECK(fx.psys->BurstDoneThisTick()[0] == 1);
        REQUIRE(raw_a->WasBurstDoneAcked() == true);

        // Add a NEW sub between ticks.  Its burst is freshly done so the
        // edge-trigger fires this tick.
        auto sub_b = fx.makeSub();
        sub_b->SetNodeId(2);
        sub_b->TEST_MarkBurstDone();
        fx.psys->subsystems.emplace_back(std::move(sub_b));

        // Tick 2: sub_a no longer fires (ack'd), sub_b fires fresh.  The
        // queue is cleared at the top of Emitt so it contains exactly {2}.
        fx.psys->Emitt();
        REQUIRE(fx.psys->BurstDoneThisTick().size() == 1u);
        CHECK(fx.psys->BurstDoneThisTick()[0] == 2);
    }

} // TEST_SUITE
