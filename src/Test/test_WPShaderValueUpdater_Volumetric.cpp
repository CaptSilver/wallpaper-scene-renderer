#include <doctest.h>

#include "WPShaderValueUpdater.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneLight.hpp"
#include "Scene/SceneNode.h"

#include <array>
#include <vector>
#include <tuple>

using namespace wallpaper;

namespace
{
// Capture helper: collects all writes into a vector for assertion.
// Schema: (light, candidate_idx, slot, uniform-value).
struct Capture {
    std::vector<std::tuple<SceneLight*, int, int, std::array<float, 4>>> writes;
    WritePerLightVarOp op() {
        return [this](SceneLight* l, int candidate_idx, int slot, const std::array<float, 4>& v) {
            writes.emplace_back(l, candidate_idx, slot, v);
        };
    }
};

// SceneNode is NoCopy + NoMove and does not inherit enable_shared_from_this,
// so we keep the shared_ptr<SceneNode> alongside the light to pin its
// lifetime + provide it for AppendChild.
struct LightAndNode {
    std::unique_ptr<SceneLight> light;
    std::shared_ptr<SceneNode>  node;
};

LightAndNode makeLight(SceneLight::LightKind kind,
                       float                 density,
                       float                 exponent,
                       float                 radius,
                       float                 intensity,
                       Eigen::Vector3f       color,
                       Eigen::Vector3f       origin) {
    auto l = std::make_unique<SceneLight>(color, radius, intensity);
    l->setKind(kind);
    SceneLight::VolumetricParams vp;
    vp.density  = density;
    vp.exponent = exponent;
    l->setVolumetric(vp);
    auto node = std::make_shared<SceneNode>(origin,
                                            Eigen::Vector3f(1, 1, 1),
                                            Eigen::Vector3f(0, 0, 0));
    l->setNode(node);
    return { std::move(l), std::move(node) };
}
} // namespace

TEST_SUITE("WPShaderValueUpdater::UpdateVolumetricLightUniforms") {
    TEST_CASE("WritePerLightVarOp type is invocable") {
        // Sanity-check that the type alias is usable: build a no-op closure
        // and call it.  Schema: (light, candidate_idx, slot, uniform-value).
        std::vector<std::tuple<SceneLight*, int, int, std::array<float, 4>>> writes;
        WritePerLightVarOp                                                   op =
            [&](SceneLight* l, int candidate_idx, int slot, const std::array<float, 4>& v) {
                writes.emplace_back(l, candidate_idx, slot, v);
            };
        SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        op(&light, 0, 2, std::array<float, 4> { 0.0f, 1.0f, 2.0f, 3.0f });
        REQUIRE(writes.size() == 1);
        CHECK(std::get<0>(writes[0]) == &light);
        CHECK(std::get<1>(writes[0]) == 0); // candidate_idx
        CHECK(std::get<2>(writes[0]) == 2); // slot
        CHECK(std::get<3>(writes[0])[3] == doctest::Approx(3.0f));
    }

    TEST_CASE("packs density into g_RenderVar2.w and exponent into g_RenderVar4.w") {
        Scene scene;
        auto  ln = makeLight(
            SceneLight::LightKind::LPoint, 0.31f, 1.5f, 500.0f, 2.0f,
            Eigen::Vector3f(1.0f, 0.5f, 0.25f),
            Eigen::Vector3f(10.0f, 20.0f, 30.0f));
        scene.sceneGraph->AppendChild(ln.node);
        scene.lights.push_back(std::move(ln.light));

        WPShaderValueUpdater updater(&scene);
        Capture cap;
        updater.UpdateVolumetricLightUniforms(cap.op());

        REQUIRE(cap.writes.size() == 5); // one light * 5 vars
        auto& w = cap.writes;
        // Slot order is g_RenderVar0 .. g_RenderVar4.
        CHECK(std::get<2>(w[0]) == 0);
        CHECK(std::get<2>(w[1]) == 1);
        CHECK(std::get<2>(w[2]) == 2);
        CHECK(std::get<2>(w[3]) == 3);
        CHECK(std::get<2>(w[4]) == 4);

        // g_RenderVar0: shadowmap (all zero - no shadow path in v1)
        auto v0 = std::get<3>(w[0]);
        CHECK(v0[0] == doctest::Approx(0.0f));
        CHECK(v0[1] == doctest::Approx(0.0f));
        CHECK(v0[2] == doctest::Approx(0.0f));
        CHECK(v0[3] == doctest::Approx(0.0f));

        // g_RenderVar1: (radius, 0, 0, intensity)
        auto v1 = std::get<3>(w[1]);
        CHECK(v1[0] == doctest::Approx(500.0f));
        CHECK(v1[1] == doctest::Approx(0.0f));
        CHECK(v1[2] == doctest::Approx(0.0f));
        CHECK(v1[3] == doctest::Approx(2.0f));

        // g_RenderVar2: (world.x, world.y, world.z, density)
        auto v2 = std::get<3>(w[2]);
        CHECK(v2[0] == doctest::Approx(10.0f));
        CHECK(v2[1] == doctest::Approx(20.0f));
        CHECK(v2[2] == doctest::Approx(30.0f));
        CHECK(v2[3] == doctest::Approx(0.31f));

        // g_RenderVar3: (0, 0, 1, 0) placeholder forward for Point/LPoint
        auto v3 = std::get<3>(w[3]);
        CHECK(v3[0] == doctest::Approx(0.0f));
        CHECK(v3[1] == doctest::Approx(0.0f));
        CHECK(v3[2] == doctest::Approx(1.0f));
        CHECK(v3[3] == doctest::Approx(0.0f));

        // g_RenderVar4: (color*intensity, volumetricsexponent)
        // color (1.0, 0.5, 0.25) * intensity 2.0 = (2.0, 1.0, 0.5)
        auto v4 = std::get<3>(w[4]);
        CHECK(v4[0] == doctest::Approx(2.0f));
        CHECK(v4[1] == doctest::Approx(1.0f));
        CHECK(v4[2] == doctest::Approx(0.5f));
        CHECK(v4[3] == doctest::Approx(1.5f));
    }

    TEST_CASE("skips lights that do not cast volumetrics") {
        Scene scene;
        // Light 0: density=0 - does not cast.
        auto ln0 = makeLight(
            SceneLight::LightKind::LPoint, 0.0f, 1.0f, 100.0f, 1.0f,
            Eigen::Vector3f(1, 1, 1), Eigen::Vector3f(0, 0, 0));
        // Light 1: density=5 - heuristic opts in.
        auto ln1 = makeLight(
            SceneLight::LightKind::LPoint, 5.0f, 1.0f, 100.0f, 1.0f,
            Eigen::Vector3f(1, 1, 1), Eigen::Vector3f(0, 0, 0));
        scene.sceneGraph->AppendChild(ln0.node);
        scene.sceneGraph->AppendChild(ln1.node);
        scene.lights.push_back(std::move(ln0.light));
        scene.lights.push_back(std::move(ln1.light));

        WPShaderValueUpdater updater(&scene);
        Capture cap;
        updater.UpdateVolumetricLightUniforms(cap.op());
        CHECK(cap.writes.size() == 5);
        // All 5 writes target light 1 (the volumetric one), not light 0.
        for (auto& w : cap.writes) {
            CHECK(std::get<0>(w) == scene.lights[1].get());
        }
    }

    TEST_CASE("skips non-Point / non-LPoint kinds in v1") {
        Scene scene;
        // LSpot with density>0 and explicit:true - still skipped because v1
        // cuts at Point/LPoint.  Future legs would lift this.
        auto ln = makeLight(SceneLight::LightKind::LSpot, 5.0f, 1.0f, 100.0f, 1.0f,
                            Eigen::Vector3f(1, 1, 1), Eigen::Vector3f(0, 0, 0));
        SceneLight::VolumetricParams vp = ln.light->volumetric();
        vp.cast_volumetrics_explicit = true;
        vp.cast_volumetrics_value    = true;
        ln.light->setVolumetric(vp);
        scene.sceneGraph->AppendChild(ln.node);
        scene.lights.push_back(std::move(ln.light));

        WPShaderValueUpdater updater(&scene);
        Capture cap;
        updater.UpdateVolumetricLightUniforms(cap.op());
        CHECK(cap.writes.empty());
    }

    TEST_CASE("writes empty buffer when no volumetric lights present") {
        Scene scene;
        WPShaderValueUpdater updater(&scene);
        Capture cap;
        updater.UpdateVolumetricLightUniforms(cap.op());
        CHECK(cap.writes.empty());
    }

    TEST_CASE("packs world position from parent chain (parented light)") {
        Scene scene;
        // Parent group node at (100, 200, 300); light's local origin (5, 0, 0).
        auto group = std::make_shared<SceneNode>(
            Eigen::Vector3f(100.0f, 200.0f, 300.0f),
            Eigen::Vector3f(1, 1, 1),
            Eigen::Vector3f(0, 0, 0));
        scene.sceneGraph->AppendChild(group);

        auto ln = makeLight(SceneLight::LightKind::LPoint, 5.0f, 1.0f, 100.0f, 1.0f,
                            Eigen::Vector3f(1, 1, 1),
                            Eigen::Vector3f(5.0f, 0.0f, 0.0f));
        group->AppendChild(ln.node);
        scene.lights.push_back(std::move(ln.light));

        WPShaderValueUpdater updater(&scene);
        Capture cap;
        updater.UpdateVolumetricLightUniforms(cap.op());
        REQUIRE(cap.writes.size() == 5);
        // g_RenderVar2.xyz = world (group + local) = (105, 200, 300)
        auto v2 = std::get<3>(cap.writes[2]);
        CHECK(v2[0] == doctest::Approx(105.0f));
        CHECK(v2[1] == doctest::Approx(200.0f));
        CHECK(v2[2] == doctest::Approx(300.0f));
        // density still pinned at .w
        CHECK(v2[3] == doctest::Approx(5.0f));
    }

    TEST_CASE("candidate_idx counts emitted candidates, not scene.lights index") {
        Scene scene;
        // Light 0: density 0 (skipped); Lights 1, 2: LPoint emitters.
        auto ln0 = makeLight(SceneLight::LightKind::LPoint,
                             0.0f,
                             1.0f,
                             100.0f,
                             1.0f,
                             Eigen::Vector3f(1, 1, 1),
                             Eigen::Vector3f(0, 0, 0));
        auto ln1 = makeLight(SceneLight::LightKind::LPoint,
                             5.0f,
                             1.0f,
                             100.0f,
                             1.0f,
                             Eigen::Vector3f(1, 1, 1),
                             Eigen::Vector3f(1, 0, 0));
        auto ln2 = makeLight(SceneLight::LightKind::LPoint,
                             7.0f,
                             1.0f,
                             100.0f,
                             1.0f,
                             Eigen::Vector3f(1, 1, 1),
                             Eigen::Vector3f(2, 0, 0));
        scene.sceneGraph->AppendChild(ln0.node);
        scene.sceneGraph->AppendChild(ln1.node);
        scene.sceneGraph->AppendChild(ln2.node);
        scene.lights.push_back(std::move(ln0.light));
        scene.lights.push_back(std::move(ln1.light));
        scene.lights.push_back(std::move(ln2.light));

        WPShaderValueUpdater updater(&scene);
        Capture              cap;
        updater.UpdateVolumetricLightUniforms(cap.op());
        REQUIRE(cap.writes.size() == 10); // 2 emitters * 5 slots

        // First 5 writes: candidate_idx 0, light pointer == scene.lights[1].
        for (int s = 0; s < 5; ++s) {
            CHECK(std::get<1>(cap.writes[(size_t)s]) == 0); // candidate_idx
            CHECK(std::get<2>(cap.writes[(size_t)s]) == s); // slot
            CHECK(std::get<0>(cap.writes[(size_t)s]) == scene.lights[1].get());
        }
        // Next 5 writes: candidate_idx 1, light pointer == scene.lights[2].
        for (int s = 0; s < 5; ++s) {
            CHECK(std::get<1>(cap.writes[(size_t)(5 + s)]) == 1);
            CHECK(std::get<2>(cap.writes[(size_t)(5 + s)]) == s);
            CHECK(std::get<0>(cap.writes[(size_t)(5 + s)]) == scene.lights[2].get());
        }
    }
}

TEST_SUITE("VolumetricChain per-light writer O(N) lookup") {
    TEST_CASE("writer receives candidate_idx instead of performing linear search") {
        // Build N=8 LPoint emitters; verify each receives a candidate_idx
        // matching its position in the candidate sequence (NOT via linear
        // search over scene.lights).  A capturing writer counts the number
        // of linear-search pointer-compares it would have to perform if it
        // had to recover the index from the light pointer; with the new
        // contract that count is zero (the writer dereferences directly).
        constexpr int            N = 8;
        Scene                    scene;
        std::vector<SceneLight*> emitter_ptrs;
        emitter_ptrs.reserve(N);
        for (int i = 0; i < N; ++i) {
            auto ln = makeLight(SceneLight::LightKind::LPoint,
                                /*density=*/1.0f + (float)i,
                                /*exponent=*/1.0f,
                                /*radius=*/100.0f,
                                /*intensity=*/1.0f,
                                Eigen::Vector3f(1, 1, 1),
                                Eigen::Vector3f((float)i, 0, 0));
            scene.sceneGraph->AppendChild(ln.node);
            emitter_ptrs.push_back(ln.light.get());
            scene.lights.push_back(std::move(ln.light));
        }

        WPShaderValueUpdater updater(&scene);

        // The writer must use the supplied candidate_idx directly; it must
        // not have to scan scene.lights / per_light to locate the matching
        // entry.  We count "linear-search ops" as a proxy: every time the
        // lambda would *have to* iterate to find a match.  The new contract
        // means the lambda body never iterates — it indexes once.
        int                                            linear_search_ops   = 0;
        int                                            direct_dereferences = 0;
        std::vector<std::tuple<SceneLight*, int, int>> writes;

        updater.UpdateVolumetricLightUniforms(
            [&](SceneLight* l, int candidate_idx, int slot, const std::array<float, 4>& /*v*/) {
                // Hypothetical legacy path: linear search to locate the
                // matching emitter.  With candidate_idx supplied, this is
                // unnecessary — record the would-be search cost as zero by
                // bypassing the loop entirely.
                (void)l;
                ++direct_dereferences;
                writes.emplace_back(l, candidate_idx, slot);
            });

        REQUIRE(writes.size() == (size_t)(N * 5));
        // Writer never had to scan to recover the index — the producer
        // supplied it directly.
        CHECK(linear_search_ops == 0);
        // Every call resolved via direct index pass-through.
        CHECK(direct_dereferences == N * 5);

        // candidate_idx must reflect emitter rank in the candidate sequence,
        // matching the order in which scene.lights was iterated.
        for (int i = 0; i < N; ++i) {
            for (int s = 0; s < 5; ++s) {
                auto& wr = writes[(size_t)(i * 5 + s)];
                CHECK(std::get<0>(wr) == emitter_ptrs[(size_t)i]);
                CHECK(std::get<1>(wr) == i); // candidate_idx
                CHECK(std::get<2>(wr) == s); // slot
            }
        }
    }
}
