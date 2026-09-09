#include <doctest.h>

#include "Particle/ParticleSystem.h"
#include "Particle/ParticleEmitter.h"
#include "Scene/Scene.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneNode.h"
#include "VulkanRender/CustomShaderPass.hpp"

#include <memory>

using namespace wallpaper;

// Turning a wallpaper feature off has to give the GPU and CPU back.  Wallpapers
// bind their own switches to layer visibility ("Rain #1 (Background)" on/off),
// so a hidden layer must stop costing a render pass and stop simulating its
// particles.  Both halves are pinned here: the pure decision the render path
// makes for a hidden pass, and the emission behaviour of a hidden system.

// ===========================================================================
// Render side — a hidden pass may drop its whole record, but only when
// dropping it leaves its render target in the state a later reader expects.
// ===========================================================================

TEST_SUITE("Hidden pass record skip") {
    using wallpaper::vulkan::CustomShaderPass;
    using wallpaper::vulkan::IsHiddenPassSkippable;
    using wallpaper::vulkan::SelectOutputLoadOp;

    TEST_CASE("a visible pass always records, whatever its load op") {
        CHECK_FALSE(IsHiddenPassSkippable(/*node_hidden=*/false, /*clears_output=*/false));
        CHECK_FALSE(IsHiddenPassSkippable(/*node_hidden=*/false, /*clears_output=*/true));
    }

    TEST_CASE("a hidden pass that only loads its target is skippable") {
        // Someone earlier in the frame already wrote (and transitioned) the
        // image, so leaving it untouched is exactly what a LOAD would have
        // produced from a draw-less pass.
        CHECK(IsHiddenPassSkippable(/*node_hidden=*/true, /*clears_output=*/false));
    }

    TEST_CASE("a hidden pass that owns its target's clear still records") {
        // Dropping the clear would hand the next pass whatever the previous
        // frame left in the image — or nothing at all on frame one.
        CHECK_FALSE(IsHiddenPassSkippable(/*node_hidden=*/true, /*clears_output=*/true));
    }

    TEST_CASE("the skip decision lines up with the prepare-time load-op choice") {
        // First writer of an RT clears it; every later writer loads.  The two
        // must agree, otherwise the skip either strands an uninitialised
        // attachment or refuses to fire for every pass in the frame.
        const bool first_writer_clears =
            SelectOutputLoadOp(/*force_clear=*/false, /*rt_already_cleared=*/false) ==
            VK_ATTACHMENT_LOAD_OP_CLEAR;
        const bool later_writer_clears =
            SelectOutputLoadOp(/*force_clear=*/false, /*rt_already_cleared=*/true) ==
            VK_ATTACHMENT_LOAD_OP_CLEAR;
        REQUIRE(first_writer_clears);
        REQUIRE_FALSE(later_writer_clears);
        CHECK_FALSE(IsHiddenPassSkippable(true, first_writer_clears));
        CHECK(IsHiddenPassSkippable(true, later_writer_clears));
    }

    TEST_CASE("Desc::clears_output defaults to false") {
        CustomShaderPass::Desc desc;
        CHECK(desc.clears_output == false);
    }
}

// ===========================================================================
// Simulation side — a hidden particle system does no work, and resuming does
// not replay everything it sat out.
// ===========================================================================

namespace
{

// Pushes exactly one long-lived particle per call, so "how many ticks did this
// system actually run" is readable straight off the particle count.
ParticleEmittOp makePushOnePerTick() {
    return [](std::vector<Particle>&       particles,
              std::vector<ParticleInitOp>& inits,
              uint32_t                     maxcount,
              double) {
        if (particles.size() >= maxcount) return;
        Particle p;
        p.lifetime = 1000.0f;
        for (auto& init : inits) init(p, 0.0);
        particles.push_back(p);
    };
}

size_t liveCount(const ParticleSubSystem& sub) {
    size_t n = 0;
    for (const auto& inst : sub.Instances()) {
        if (inst) n += inst->Particles().size();
    }
    return n;
}

// Scene graph with one node that draws the subsystem's mesh — the shape the
// parser produces for a particle object.
struct HiddenLayerFixture {
    Scene                           scene;
    std::unique_ptr<ParticleSystem> psys;
    std::shared_ptr<SceneMesh>      mesh;
    std::shared_ptr<SceneNode>      node;
    ParticleSubSystem*              sub { nullptr };

    HiddenLayerFixture() {
        psys = std::make_unique<ParticleSystem>(scene);
        mesh = std::make_shared<SceneMesh>(true);
        node = std::make_shared<SceneNode>();
        node->AddMesh(mesh);
        scene.sceneGraph->AppendChild(node);

        auto owned = std::make_unique<ParticleSubSystem>(*psys,
                                                         mesh,
                                                         /*maxcount=*/100,
                                                         /*rate=*/1.0,
                                                         /*maxcount_instance=*/1,
                                                         /*probability=*/1.0,
                                                         ParticleSubSystem::SpawnType::STATIC,
                                                         nullptr,
                                                         /*starttime=*/0.0f);
        owned->AddEmitter(makePushOnePerTick());
        sub = owned.get();
        psys->subsystems.push_back(std::move(owned));
    }

    void tick(int n = 1) {
        for (int i = 0; i < n; i++) {
            scene.PassFrameTime(0.016);
            psys->Emitt();
        }
    }
};

} // namespace

TEST_SUITE("Hidden particle system") {
    TEST_CASE("a visible system emits once per tick") {
        HiddenLayerFixture fx;
        fx.tick(3);
        CHECK(liveCount(*fx.sub) == 3);
    }

    TEST_CASE("a hidden system emits nothing") {
        HiddenLayerFixture fx;
        fx.node->SetVisible(false);
        fx.tick(5);
        CHECK(liveCount(*fx.sub) == 0);
    }

    TEST_CASE("a hidden system stops after having been visible") {
        HiddenLayerFixture fx;
        fx.tick(2);
        REQUIRE(liveCount(*fx.sub) == 2);
        fx.node->SetVisible(false);
        fx.tick(10);
        CHECK(liveCount(*fx.sub) == 2);
    }

    TEST_CASE("becoming visible again resumes at one tick, not a catch-up burst") {
        HiddenLayerFixture fx;
        fx.node->SetVisible(false);
        fx.tick(10);
        fx.node->SetVisible(true);
        fx.tick(1);
        CHECK(liveCount(*fx.sub) == 1);
    }

    TEST_CASE("a hidden parent node stops its children too") {
        HiddenLayerFixture fx;
        auto               childMesh = std::make_shared<SceneMesh>(true);
        auto               childNode = std::make_shared<SceneNode>();
        childNode->AddMesh(childMesh);
        fx.node->AppendChild(childNode);

        auto child = std::make_unique<ParticleSubSystem>(*fx.psys,
                                                         childMesh,
                                                         /*maxcount=*/100,
                                                         /*rate=*/1.0,
                                                         /*maxcount_instance=*/1,
                                                         /*probability=*/1.0,
                                                         ParticleSubSystem::SpawnType::STATIC,
                                                         nullptr,
                                                         /*starttime=*/0.0f);
        child->AddEmitter(makePushOnePerTick());
        auto* child_raw = child.get();
        fx.sub->AddChild(std::move(child));

        fx.node->SetVisible(false);
        fx.tick(4);
        CHECK(liveCount(*child_raw) == 0);
    }

    TEST_CASE("a subsystem whose mesh no node draws keeps simulating") {
        // Spawner-only subsystems never get their mesh attached to the graph;
        // they have no visibility to consult and must not be silently frozen.
        HiddenLayerFixture fx;
        auto               detachedMesh = std::make_shared<SceneMesh>(true);
        auto detached = std::make_unique<ParticleSubSystem>(*fx.psys,
                                                            detachedMesh,
                                                            /*maxcount=*/100,
                                                            /*rate=*/1.0,
                                                            /*maxcount_instance=*/1,
                                                            /*probability=*/1.0,
                                                            ParticleSubSystem::SpawnType::STATIC,
                                                            nullptr,
                                                            /*starttime=*/0.0f);
        detached->AddEmitter(makePushOnePerTick());
        auto* detached_raw = detached.get();
        fx.psys->subsystems.push_back(std::move(detached));

        fx.node->SetVisible(false);
        fx.tick(3);
        CHECK(liveCount(*detached_raw) == 3);
    }
}
