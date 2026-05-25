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
struct Capture {
    std::vector<std::tuple<SceneLight*, int, std::array<float, 4>>> writes;
    WritePerLightVarOp op() {
        return [this](SceneLight* l, int slot, const std::array<float, 4>& v) {
            writes.emplace_back(l, slot, v);
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
        // and call it.
        std::vector<std::tuple<SceneLight*, int, std::array<float, 4>>> writes;
        WritePerLightVarOp op = [&](SceneLight* l, int slot,
                                    const std::array<float, 4>& v) {
            writes.emplace_back(l, slot, v);
        };
        SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        op(&light, 2, std::array<float, 4> { 0.0f, 1.0f, 2.0f, 3.0f });
        REQUIRE(writes.size() == 1);
        CHECK(std::get<0>(writes[0]) == &light);
        CHECK(std::get<1>(writes[0]) == 2);
        CHECK(std::get<2>(writes[0])[3] == doctest::Approx(3.0f));
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
        CHECK(std::get<1>(w[0]) == 0);
        CHECK(std::get<1>(w[1]) == 1);
        CHECK(std::get<1>(w[2]) == 2);
        CHECK(std::get<1>(w[3]) == 3);
        CHECK(std::get<1>(w[4]) == 4);

        // g_RenderVar0: shadowmap (all zero - no shadow path in v1)
        auto v0 = std::get<2>(w[0]);
        CHECK(v0[0] == doctest::Approx(0.0f));
        CHECK(v0[1] == doctest::Approx(0.0f));
        CHECK(v0[2] == doctest::Approx(0.0f));
        CHECK(v0[3] == doctest::Approx(0.0f));

        // g_RenderVar1: (radius, 0, 0, intensity)
        auto v1 = std::get<2>(w[1]);
        CHECK(v1[0] == doctest::Approx(500.0f));
        CHECK(v1[1] == doctest::Approx(0.0f));
        CHECK(v1[2] == doctest::Approx(0.0f));
        CHECK(v1[3] == doctest::Approx(2.0f));

        // g_RenderVar2: (world.x, world.y, world.z, density)
        auto v2 = std::get<2>(w[2]);
        CHECK(v2[0] == doctest::Approx(10.0f));
        CHECK(v2[1] == doctest::Approx(20.0f));
        CHECK(v2[2] == doctest::Approx(30.0f));
        CHECK(v2[3] == doctest::Approx(0.31f));

        // g_RenderVar3: (0, 0, 1, 0) placeholder forward for Point/LPoint
        auto v3 = std::get<2>(w[3]);
        CHECK(v3[0] == doctest::Approx(0.0f));
        CHECK(v3[1] == doctest::Approx(0.0f));
        CHECK(v3[2] == doctest::Approx(1.0f));
        CHECK(v3[3] == doctest::Approx(0.0f));

        // g_RenderVar4: (color*intensity, volumetricsexponent)
        // color (1.0, 0.5, 0.25) * intensity 2.0 = (2.0, 1.0, 0.5)
        auto v4 = std::get<2>(w[4]);
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
        auto v2 = std::get<2>(cap.writes[2]);
        CHECK(v2[0] == doctest::Approx(105.0f));
        CHECK(v2[1] == doctest::Approx(200.0f));
        CHECK(v2[2] == doctest::Approx(300.0f));
        // density still pinned at .w
        CHECK(v2[3] == doctest::Approx(5.0f));
    }
}
