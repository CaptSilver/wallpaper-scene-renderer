#include <doctest.h>

#include "VulkanRender/VolumetricChain.hpp"
#include "VulkanRender/CustomShaderPass.hpp"
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneLight.hpp"
#include "Scene/SceneNode.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneMaterial.h"
#include "SpecTexs.hpp"
#include "WPShaderValueUpdater.hpp"
#include "wpscene/WPMaterial.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

using namespace wallpaper;

// Forward-decl of the icosphere populator (lives at namespace wallpaper scope
// in WPSceneParser.cpp).  Cloned into each per-light back/front proxy by
// BuildVolumetricNodes.
namespace wallpaper
{
void GenVolumeSphereMesh(SceneMesh& mesh);
}

namespace
{

// Minimal scene with N volumetric Point lights at staggered local positions
// (x = i * 100).  Camera placed far away so all lights are OUTSIDE — the
// inside/outside-selection assertions live in a separate suite covered by the
// per-frame tick.
struct VolumetricSceneFixture {
    Scene                                   scene;
    std::shared_ptr<SceneCamera>            cam;
    std::vector<std::shared_ptr<SceneNode>> light_nodes;

    void buildLights(size_t n, float radius = 10.0f) {
        // Populate default_volume_sphere so BuildVolumetricNodes can clone it.
        // Bare Scene ctor leaves it empty; production goes through InitContext
        // (WPSceneParser).
        wallpaper::GenVolumeSphereMesh(scene.default_volume_sphere);
        // default_effect_mesh remains empty for these topology tests; the
        // fullscreen-quad data is not required for node-construction
        // assertions.

        for (size_t i = 0; i < n; i++) {
            auto node = std::make_shared<SceneNode>(
                Eigen::Vector3f { static_cast<float>(i) * 100.0f, 0.0f, 0.0f },
                Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
                Eigen::Vector3f { 0.0f, 0.0f, 0.0f });
            light_nodes.push_back(node);

            auto light = std::make_unique<SceneLight>(
                Eigen::Vector3f { 1.0f, 1.0f, 1.0f }, radius, 1.0f, 1.0f);
            light->setKind(SceneLight::LightKind::LPoint);
            SceneLight::VolumetricParams vp;
            vp.density = 1.0f;
            light->setVolumetric(vp);
            light->setNode(node);

            Scene::VolumetricsConfig::PerLight pl;
            pl.light = light.get();
            scene.volumetricsConfig.per_light.push_back(std::move(pl));
            scene.lights.push_back(std::move(light));
        }
        scene.volumetricsConfig.enabled = ! scene.volumetricsConfig.per_light.empty();

        cam = std::make_shared<SceneCamera>(1920, 1080, 0.1f, 1000.0f);
        cam->SetDirectLookAt(Eigen::Vector3d { 0.0, 0.0, -100000.0 },
                             Eigen::Vector3d { 0.0, 0.0, 0.0 },
                             Eigen::Vector3d { 0.0, 1.0, 0.0 });
        scene.cameras["default"] = cam;
        scene.activeCamera       = cam.get();
    }
};

} // namespace

TEST_SUITE("SceneToRenderGraph_Volumetrics_NodeBuild") {
    TEST_CASE("per-light back/front/fullscreen nodes constructed at build time") {
        VolumetricSceneFixture f;
        f.buildLights(2);
        vulkan::BuildVolumetricNodes(f.scene);

        REQUIRE(f.scene.volumetricsConfig.per_light.size() == 2u);
        for (auto& pl : f.scene.volumetricsConfig.per_light) {
            CHECK(pl.back_node != nullptr);
            CHECK(pl.front_node != nullptr);
            CHECK(pl.fullscreen_node != nullptr);
        }
        CHECK(f.scene.volumetricsConfig.blur_h_node != nullptr);
        CHECK(f.scene.volumetricsConfig.blur_v_node != nullptr);
        CHECK(f.scene.volumetricsConfig.combine_node != nullptr);
    }

    TEST_CASE("per-light back_node uses radius-scaled transform at light world origin") {
        VolumetricSceneFixture f;
        f.buildLights(1, /*radius=*/42.0f);
        vulkan::BuildVolumetricNodes(f.scene);
        auto& pl = f.scene.volumetricsConfig.per_light[0];
        REQUIRE(pl.back_node != nullptr);
        pl.back_node->UpdateTrans();
        const auto& m = pl.back_node->ModelTrans();
        // Light's local position (i=0) at (0,0,0) → world origin (0,0,0).
        CHECK(m(0, 3) == doctest::Approx(0.0));
        CHECK(m(1, 3) == doctest::Approx(0.0));
        CHECK(m(2, 3) == doctest::Approx(0.0));
        // Uniform scale magnitude on the X axis should equal radius.
        const double scale_x = std::sqrt(m(0, 0) * m(0, 0) + m(1, 0) * m(1, 0) + m(2, 0) * m(2, 0));
        CHECK(scale_x == doctest::Approx(42.0).epsilon(0.01));
    }

    TEST_CASE("registers half-res render-target entries for the three RT keys") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        auto* back = f.scene.tryGetRenderTarget(std::string(WE_VOLUMETRICS_BACK));
        auto* lb   = f.scene.tryGetRenderTarget(std::string(WE_VOLUMETRICS_LIGHT_BUFFER));
        auto* lbb  = f.scene.tryGetRenderTarget(std::string(WE_VOLUMETRICS_LIGHT_BUFFER_B));
        REQUIRE(back != nullptr);
        REQUIRE(lb != nullptr);
        REQUIRE(lbb != nullptr);
        CHECK(back->bind.scale == doctest::Approx(0.5));
        CHECK(lb->bind.scale == doctest::Approx(0.5));
        CHECK(lbb->bind.scale == doctest::Approx(0.5));
        CHECK(back->bind.enable == true);
        CHECK(lb->bind.enable == true);
        CHECK(lbb->bind.enable == true);
        // Volumetric scatter is additive across N lights; RGBA8 would clip /
        // band before the combine pass had a chance to tonemap.
        CHECK(back->format == wallpaper::TextureFormat::RGBA16F);
        CHECK(lb->format == wallpaper::TextureFormat::RGBA16F);
        CHECK(lbb->format == wallpaper::TextureFormat::RGBA16F);
    }

    TEST_CASE("no volumetric lights → no node construction, no crash") {
        VolumetricSceneFixture f;
        // No buildLights — per_light empty.
        wallpaper::GenVolumeSphereMesh(f.scene.default_volume_sphere);
        vulkan::BuildVolumetricNodes(f.scene);
        CHECK(f.scene.volumetricsConfig.per_light.empty());
        CHECK(f.scene.volumetricsConfig.blur_h_node == nullptr);
        CHECK(f.scene.volumetricsConfig.blur_v_node == nullptr);
        CHECK(f.scene.volumetricsConfig.combine_node == nullptr);
        // Half-res RTs should NOT be registered when there's nothing to render.
        CHECK(f.scene.tryGetRenderTarget(std::string(WE_VOLUMETRICS_BACK)) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// Helpers shared across the per-pass emission test suites.  Walk the graph in
// topological order and find the single pass whose PassNode name matches.
// Returns nullptr on miss; tests REQUIRE-non-null where the pass is expected.
// ---------------------------------------------------------------------------
namespace
{

vulkan::CustomShaderPass* findPassByName(rg::RenderGraph& rgraph, std::string_view name) {
    for (auto id : rgraph.topologicalOrder()) {
        auto* pn = rgraph.getPassNode(id);
        if (! pn) continue;
        if (pn->name() != name) continue;
        return dynamic_cast<vulkan::CustomShaderPass*>(rgraph.getPass(id));
    }
    return nullptr;
}

std::vector<std::string> collectPassNames(rg::RenderGraph& rgraph) {
    std::vector<std::string> names;
    for (auto id : rgraph.topologicalOrder()) {
        auto* pn = rgraph.getPassNode(id);
        if (! pn) continue;
        names.emplace_back(pn->name());
    }
    return names;
}

bool textureListContains(const std::vector<std::string>& textures, std::string_view key) {
    return std::find(textures.begin(), textures.end(), std::string(key)) != textures.end();
}

} // namespace

TEST_SUITE("SceneToRenderGraph_Volumetrics_BackPass") {
    TEST_CASE("emits a single pass named volumetrics_back_<idx>") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricBackPass(rgraph, f.scene, 0, extra);

        auto names = collectPassNames(rgraph);
        REQUIRE(names.size() == 1u);
        CHECK(names[0] == "volumetrics_back_0");
    }

    TEST_CASE("back pass writes WE_VOLUMETRICS_BACK with force_clear_output") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricBackPass(rgraph, f.scene, 0, extra);

        auto* csp = findPassByName(rgraph, "volumetrics_back_0");
        REQUIRE(csp != nullptr);
        CHECK(csp->desc().output == std::string(WE_VOLUMETRICS_BACK));
        CHECK(csp->desc().force_clear_output == true);
    }

    TEST_CASE("back pass needsSceneDepth=true and flipCullMode=true (back-face raster)") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricBackPass(rgraph, f.scene, 0, extra);

        auto* csp = findPassByName(rgraph, "volumetrics_back_0");
        REQUIRE(csp != nullptr);
        CHECK(csp->desc().needsSceneDepth == true);
        CHECK(csp->desc().flipCullMode == true);
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_FrontPass") {
    TEST_CASE("emits a single pass named volumetrics_front_<idx> writing the light buffer") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricFrontPass(rgraph, f.scene, 0, extra);

        auto names = collectPassNames(rgraph);
        REQUIRE(names.size() == 1u);
        CHECK(names[0] == "volumetrics_front_0");

        auto* csp = findPassByName(rgraph, "volumetrics_front_0");
        REQUIRE(csp != nullptr);
        CHECK(csp->desc().output == std::string(WE_VOLUMETRICS_LIGHT_BUFFER));
    }

    TEST_CASE("front pass samples WE_VOLUMETRICS_BACK + needsSceneDepth (per-light ray-march)") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricFrontPass(rgraph, f.scene, 0, extra);

        auto* csp = findPassByName(rgraph, "volumetrics_front_0");
        REQUIRE(csp != nullptr);
        CHECK(textureListContains(csp->desc().textures, WE_VOLUMETRICS_BACK));
        CHECK(csp->desc().needsSceneDepth == true);
    }

    TEST_CASE("front pass: no force_clear_output, no flipCullMode (additive front-face raster)") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricFrontPass(rgraph, f.scene, 0, extra);

        auto* csp = findPassByName(rgraph, "volumetrics_front_0");
        REQUIRE(csp != nullptr);
        CHECK(csp->desc().force_clear_output == false);
        CHECK(csp->desc().flipCullMode == false);
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_FullscreenPass") {
    TEST_CASE("emits volumetrics_fullscreen_<idx> writing the same light buffer as front") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricFullscreenPass(rgraph, f.scene, 0, extra);

        auto names = collectPassNames(rgraph);
        REQUIRE(names.size() == 1u);
        CHECK(names[0] == "volumetrics_fullscreen_0");

        auto* csp = findPassByName(rgraph, "volumetrics_fullscreen_0");
        REQUIRE(csp != nullptr);
        CHECK(csp->desc().output == std::string(WE_VOLUMETRICS_LIGHT_BUFFER));
    }

    TEST_CASE("fullscreen pass sets disableDepth=true (no depth test for fullscreen quad)") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricFullscreenPass(rgraph, f.scene, 0, extra);

        auto* csp = findPassByName(rgraph, "volumetrics_fullscreen_0");
        REQUIRE(csp != nullptr);
        CHECK(csp->desc().disableDepth == true);
        CHECK(csp->desc().needsSceneDepth == true);
        CHECK(csp->desc().force_clear_output == false);
        CHECK(textureListContains(csp->desc().textures, WE_VOLUMETRICS_BACK));
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_PerLightSelection") {
    TEST_CASE("camera outside → back + front, NO fullscreen") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        f.scene.TickVolumetricSelection();
        REQUIRE(f.scene.volumetricsConfig.per_light[0].is_inside_this_frame == false);

        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricPerLight(rgraph, f.scene, 0, extra);

        auto names = collectPassNames(rgraph);
        REQUIRE(names.size() == 2u);
        CHECK(names[0] == "volumetrics_back_0");
        CHECK(names[1] == "volumetrics_front_0");
        // Explicit absence guard — no fullscreen pass emitted.
        CHECK(findPassByName(rgraph, "volumetrics_fullscreen_0") == nullptr);
    }

    TEST_CASE("camera inside → back + fullscreen, NO front") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        // Bypass the camera-distance flip — the fixture's camera is far away
        // by default, and rebuilding it to live inside the per-light proxy is
        // a lot of fuss for a one-bit selection assertion.
        f.scene.volumetricsConfig.per_light[0].is_inside_this_frame = true;

        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricPerLight(rgraph, f.scene, 0, extra);

        auto names = collectPassNames(rgraph);
        REQUIRE(names.size() == 2u);
        CHECK(names[0] == "volumetrics_back_0");
        CHECK(names[1] == "volumetrics_fullscreen_0");
        CHECK(findPassByName(rgraph, "volumetrics_front_0") == nullptr);
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_BlurPasses") {
    // Run a per-light pass before the blur so the LIGHT_BUFFER TexNode has a
    // writer at blur_h's read site.  This matches the production emission
    // order (per-light → blur → combine); skipping it makes blur_v's write
    // re-use the version blur_h read, which RenderGraphBuilder detects as a
    // self-cycle and logs an ERROR.  Production never hits that path because
    // emitVolumetricChain always emits per-light before blur.
    TEST_CASE("blur_h then blur_v emit after the per-light passes") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricPerLight(rgraph, f.scene, 0, extra);
        vulkan::emitVolumetricBlurPasses(rgraph, f.scene, extra);

        auto names = collectPassNames(rgraph);
        REQUIRE(names.size() == 4u);
        CHECK(names[0] == "volumetrics_back_0");
        CHECK(names[1] == "volumetrics_front_0");
        CHECK(names[2] == "volumetrics_blur_h");
        CHECK(names[3] == "volumetrics_blur_v");
    }

    TEST_CASE("blur_h reads LIGHT_BUFFER → writes LIGHT_BUFFER_B; blur_v reverses") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricPerLight(rgraph, f.scene, 0, extra);
        vulkan::emitVolumetricBlurPasses(rgraph, f.scene, extra);

        auto* h = findPassByName(rgraph, "volumetrics_blur_h");
        auto* v = findPassByName(rgraph, "volumetrics_blur_v");
        REQUIRE(h != nullptr);
        REQUIRE(v != nullptr);
        CHECK(h->desc().output == std::string(WE_VOLUMETRICS_LIGHT_BUFFER_B));
        CHECK(textureListContains(h->desc().textures, WE_VOLUMETRICS_LIGHT_BUFFER));
        CHECK(v->desc().output == std::string(WE_VOLUMETRICS_LIGHT_BUFFER));
        CHECK(textureListContains(v->desc().textures, WE_VOLUMETRICS_LIGHT_BUFFER_B));
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_CombinePass") {
    TEST_CASE("volumetrics_combine writes _rt_default and reads WE_VOLUMETRICS_LIGHT_BUFFER") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricCombinePass(rgraph, f.scene, extra);

        auto names = collectPassNames(rgraph);
        REQUIRE(names.size() == 1u);
        CHECK(names[0] == "volumetrics_combine");

        auto* csp = findPassByName(rgraph, "volumetrics_combine");
        REQUIRE(csp != nullptr);
        CHECK(csp->desc().output == std::string(SpecTex_Default));
        CHECK(textureListContains(csp->desc().textures, WE_VOLUMETRICS_LIGHT_BUFFER));
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_Chain") {
    TEST_CASE("zero volumetric lights → no volumetric passes") {
        VolumetricSceneFixture f;
        // Don't call buildLights — per_light stays empty.  Materialise camera
        // by hand so the chain entry point has something to test against.
        f.cam = std::make_shared<SceneCamera>(1920, 1080, 0.1f, 1000.0f);
        f.cam->SetDirectLookAt(Eigen::Vector3d { 0.0, 0.0, -100000.0 },
                               Eigen::Vector3d { 0.0, 0.0, 0.0 },
                               Eigen::Vector3d { 0.0, 1.0, 0.0 });
        f.scene.cameras["default"] = f.cam;
        f.scene.activeCamera       = f.cam.get();
        // Explicit: cover both predicate guards (enabled false + per_light empty).
        f.scene.volumetricsConfig.enabled = false;

        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricChain(rgraph, f.scene, *f.cam, extra);

        auto names = collectPassNames(rgraph);
        CHECK(names.empty());
    }

    TEST_CASE("one volumetric light outside → 5 passes back_0/front_0/blur_h/blur_v/combine") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        f.scene.TickVolumetricSelection();
        REQUIRE(f.scene.volumetricsConfig.per_light[0].is_inside_this_frame == false);

        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricChain(rgraph, f.scene, *f.cam, extra);

        auto names = collectPassNames(rgraph);
        REQUIRE(names.size() == 5u);
        CHECK(names[0] == "volumetrics_back_0");
        CHECK(names[1] == "volumetrics_front_0");
        CHECK(names[2] == "volumetrics_blur_h");
        CHECK(names[3] == "volumetrics_blur_v");
        CHECK(names[4] == "volumetrics_combine");
    }

    TEST_CASE("three volumetric lights outside → 9 passes 3×(back/front)+blur_h/v+combine") {
        VolumetricSceneFixture f;
        f.buildLights(3);
        vulkan::BuildVolumetricNodes(f.scene);
        f.scene.TickVolumetricSelection();
        for (auto& pl : f.scene.volumetricsConfig.per_light) {
            REQUIRE(pl.is_inside_this_frame == false);
        }

        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricChain(rgraph, f.scene, *f.cam, extra);

        auto names = collectPassNames(rgraph);
        REQUIRE(names.size() == 9u);
        CHECK(names[0] == "volumetrics_back_0");
        CHECK(names[1] == "volumetrics_front_0");
        CHECK(names[2] == "volumetrics_back_1");
        CHECK(names[3] == "volumetrics_front_1");
        CHECK(names[4] == "volumetrics_back_2");
        CHECK(names[5] == "volumetrics_front_2");
        CHECK(names[6] == "volumetrics_blur_h");
        CHECK(names[7] == "volumetrics_blur_v");
        CHECK(names[8] == "volumetrics_combine");
    }

    TEST_CASE("reflection-pass camera → zero volumetric passes emitted") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        f.scene.TickVolumetricSelection();
        // Reflection emissions reuse the main scene-depth target keyed for the
        // non-reflected view; the volumetric ray-march would integrate against
        // the wrong depth buffer.  Skip the chain entirely.
        f.cam->SetReflectY0(true);

        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricChain(rgraph, f.scene, *f.cam, extra);

        auto names = collectPassNames(rgraph);
        CHECK(names.empty());
    }

    TEST_CASE("enabled=false + per_light empty (force-off post-parse state) → zero passes") {
        VolumetricSceneFixture f;
        f.cam = std::make_shared<SceneCamera>(1920, 1080, 0.1f, 1000.0f);
        f.cam->SetDirectLookAt(Eigen::Vector3d { 0.0, 0.0, -100000.0 },
                               Eigen::Vector3d { 0.0, 0.0, 0.0 },
                               Eigen::Vector3d { 0.0, 1.0, 0.0 });
        f.scene.cameras["default"] = f.cam;
        f.scene.activeCamera       = f.cam.get();
        f.scene.volumetricsConfig.enabled = false;
        REQUIRE(f.scene.volumetricsConfig.per_light.empty());

        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricChain(rgraph, f.scene, *f.cam, extra);

        auto names = collectPassNames(rgraph);
        CHECK(names.empty());
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_SingleAlias") {
    TEST_CASE("WE_VOLUMETRICS_SINGLE is registered with the _rt_ prefix") {
        CHECK(WE_VOLUMETRICS_SINGLE == std::string_view("_rt_volumetricsSingle"));
        CHECK(IsSpecTex(WE_VOLUMETRICS_SINGLE));
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_MsaaGate") {
    TEST_CASE("MSAA > 1 -> zero volumetric passes emitted (v1 incompatibility)") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        f.scene.TickVolumetricSelection();
        f.scene.msaaSamples = 4;

        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricChain(rgraph, f.scene, *f.cam, extra);

        auto names = collectPassNames(rgraph);
        bool any_vol = std::any_of(names.begin(), names.end(),
                                    [](const std::string& s) {
                                        return s.rfind("volumetrics_", 0) == 0;
                                    });
        CHECK_FALSE(any_vol);
    }

    TEST_CASE("MSAA == 1 -> chain emits as normal (baseline for MSAA gate)") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        f.scene.TickVolumetricSelection();
        f.scene.msaaSamples = 1;

        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricChain(rgraph, f.scene, *f.cam, extra);

        auto names = collectPassNames(rgraph);
        CHECK(names.size() == 5u);
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_Topology") {
    TEST_CASE("integrated chain (1 light) topological order: back_0 < front_0 < blur_h < blur_v < combine") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        f.scene.TickVolumetricSelection();
        REQUIRE(f.scene.volumetricsConfig.per_light[0].is_inside_this_frame == false);

        rg::RenderGraph              rgraph;
        vulkan::ExtraInfoVolumetrics extra { .scene = &f.scene, .rgraph = &rgraph };
        vulkan::emitVolumetricChain(rgraph, f.scene, *f.cam, extra);

        auto names = collectPassNames(rgraph);
        REQUIRE(names.size() == 5u);
        auto pos = [&names](const std::string& n) {
            return std::find(names.begin(), names.end(), n) - names.begin();
        };
        const auto back_0  = pos("volumetrics_back_0");
        const auto front_0 = pos("volumetrics_front_0");
        const auto blur_h  = pos("volumetrics_blur_h");
        const auto blur_v  = pos("volumetrics_blur_v");
        const auto combine = pos("volumetrics_combine");
        // Combine sits after blur_v, blur_v after blur_h, blur_h after every
        // per-light front/back; back_i before front_i for each light.
        CHECK(back_0 < front_0);
        CHECK(front_0 < blur_h);
        CHECK(blur_h < blur_v);
        CHECK(blur_v < combine);
    }
}

TEST_SUITE("VolumetricChain_PumpVolumetricFrame") {
    TEST_CASE("disabled chain — no-op (no constValues written)") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        // Attach a stub material on the front node so we could observe writes
        // if they were attempted.  An empty SceneMaterial has empty
        // constValues; we assert it stays empty.
        f.scene.volumetricsConfig.per_light[0].front_node->Mesh()->AddMaterial(SceneMaterial {});

        // Disable the chain after BuildVolumetricNodes (the fixture turns it
        // on once per_light has entries).
        f.scene.volumetricsConfig.enabled = false;

        WPShaderValueUpdater updater(&f.scene);
        vulkan::PumpVolumetricFrame(f.scene, updater);

        // No writes attempted → constValues stays empty.
        CHECK(f.scene.volumetricsConfig.per_light[0]
                  .front_node->Mesh()
                  ->Material()
                  ->customShader.constValues.empty());
    }

    TEST_CASE("enabled chain — TickVolumetricSelection runs AND constValues populated on front material") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        // Attach stub materials on both per-light surface nodes so the writer
        // can target both (it does — see PumpVolumetricFrame body comment).
        f.scene.volumetricsConfig.per_light[0].front_node->Mesh()->AddMaterial(SceneMaterial {});
        f.scene.volumetricsConfig.per_light[0]
            .fullscreen_node->Mesh()
            ->AddMaterial(SceneMaterial {});
        REQUIRE(f.scene.volumetricsConfig.enabled == true);

        WPShaderValueUpdater updater(&f.scene);
        vulkan::PumpVolumetricFrame(f.scene, updater);

        // Tick result observable: camera placed far away by fixture → outside
        // the light's radius.
        CHECK(f.scene.volumetricsConfig.per_light[0].is_inside_this_frame == false);

        // All 5 g_RenderVar slots populated on the front material.
        auto& front_mat =
            *f.scene.volumetricsConfig.per_light[0].front_node->Mesh()->Material();
        for (int i = 0; i <= 4; i++) {
            std::string name = "g_RenderVar" + std::to_string(i);
            CHECK(front_mat.customShader.constValues.count(name) == 1u);
        }
        CHECK(front_mat.customShader.constValuesDirty == true);

        // Fullscreen material parallels front (writer targets both so the
        // unused one is ready when the camera flips into the volume).
        auto& fs_mat = *f.scene.volumetricsConfig.per_light[0]
                            .fullscreen_node->Mesh()
                            ->Material();
        CHECK(fs_mat.customShader.constValues.size() == 5u);
    }

    TEST_CASE("missing per-light material — no-op, no crash") {
        VolumetricSceneFixture f;
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        // DO NOT attach materials — matches the pre-material-wire state where
        // the volumetric per-light nodes have a mesh but no SceneMaterial.
        REQUIRE(f.scene.volumetricsConfig.per_light[0]
                    .front_node->Mesh()
                    ->Material() == nullptr);

        WPShaderValueUpdater updater(&f.scene);
        vulkan::PumpVolumetricFrame(f.scene, updater);

        // No crash is the assertion.  Re-state material is still null.
        CHECK(f.scene.volumetricsConfig.per_light[0]
                  .front_node->Mesh()
                  ->Material() == nullptr);
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_QualityMapping") {
    TEST_CASE("ultra tier maps to QUALITY=4") {
        CHECK(vulkan::volumetric_quality_from_pp_tier("ultra") == 4u);
    }
    TEST_CASE("displayhdr tier maps to QUALITY=4") {
        CHECK(vulkan::volumetric_quality_from_pp_tier("displayhdr") == 4u);
    }
    TEST_CASE("medium tier maps to QUALITY=2") {
        CHECK(vulkan::volumetric_quality_from_pp_tier("medium") == 2u);
    }
    TEST_CASE("low tier maps to QUALITY=1") {
        CHECK(vulkan::volumetric_quality_from_pp_tier("low") == 1u);
    }
    TEST_CASE("empty tier defaults to QUALITY=2 (medium)") {
        CHECK(vulkan::volumetric_quality_from_pp_tier("") == 2u);
    }
    TEST_CASE("unrecognised tier defaults to QUALITY=2 (defensive)") {
        CHECK(vulkan::volumetric_quality_from_pp_tier("garbage") == 2u);
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_BuildersWPMaterial") {
    TEST_CASE("back material: front-cull translucent depth-enabled writes none") {
        const auto m = vulkan::buildVolumetricBackMaterial(4);
        CHECK(m.shader == "volumetricsback");
        CHECK(m.cullmode == "front");
        CHECK(m.blending == "normal");
        CHECK(m.depthtest == "enabled");
        CHECK(m.depthwrite == "disabled");
        REQUIRE(m.combos.count("QUALITY") == 1u);
        CHECK(m.combos.at("QUALITY") == 4);
    }

    TEST_CASE("front material: back-cull additive ray-march combos populated") {
        const auto m = vulkan::buildVolumetricFrontMaterial(4);
        CHECK(m.shader == "volumetricsfront");
        CHECK(m.cullmode == "back");
        CHECK(m.blending == "additive");
        CHECK(m.depthtest == "disabled");
        CHECK(m.depthwrite == "disabled");
        REQUIRE(m.combos.count("QUALITY") == 1u);
        CHECK(m.combos.at("QUALITY") == 4);
        REQUIRE(m.combos.count("POINTLIGHT") == 1u);
        CHECK(m.combos.at("POINTLIGHT") == 1);
        REQUIRE(m.combos.count("FULLSCREEN") == 1u);
        CHECK(m.combos.at("FULLSCREEN") == 0);
        REQUIRE(m.combos.count("SHADOW") == 1u);
        CHECK(m.combos.at("SHADOW") == 0);
        REQUIRE(m.combos.count("REVERSEDEPTH") == 1u);
        CHECK(m.combos.at("REVERSEDEPTH") == 0);
        // Samples the back-depth RT to integrate front → back.
        REQUIRE_FALSE(m.textures.empty());
        CHECK(m.textures[0] == std::string(WE_VOLUMETRICS_BACK));
    }

    TEST_CASE("fullscreen material: nocull + FULLSCREEN=1 + QUALITY threaded through") {
        const auto m = vulkan::buildVolumetricFullscreenMaterial(2);
        CHECK(m.shader == "volumetricsfront");
        CHECK(m.cullmode == "nocull");
        CHECK(m.blending == "additive");
        REQUIRE(m.combos.count("FULLSCREEN") == 1u);
        CHECK(m.combos.at("FULLSCREEN") == 1);
        REQUIRE(m.combos.count("QUALITY") == 1u);
        CHECK(m.combos.at("QUALITY") == 2);
        REQUIRE_FALSE(m.textures.empty());
        CHECK(m.textures[0] == std::string(WE_VOLUMETRICS_BACK));
    }

    TEST_CASE("blur materials: blur_k3 with VERTICAL=0/1 on the matching ping-pong RT") {
        const auto h = vulkan::buildVolumetricBlurMaterial(0);
        CHECK(h.shader == "blur_k3");
        CHECK(h.cullmode == "nocull");
        CHECK(h.blending == "normal");
        REQUIRE(h.combos.count("VERTICAL") == 1u);
        CHECK(h.combos.at("VERTICAL") == 0);
        REQUIRE_FALSE(h.textures.empty());
        CHECK(h.textures[0] == std::string(WE_VOLUMETRICS_LIGHT_BUFFER));

        const auto v = vulkan::buildVolumetricBlurMaterial(1);
        CHECK(v.shader == "blur_k3");
        REQUIRE(v.combos.count("VERTICAL") == 1u);
        CHECK(v.combos.at("VERTICAL") == 1);
        REQUIRE_FALSE(v.textures.empty());
        CHECK(v.textures[0] == std::string(WE_VOLUMETRICS_LIGHT_BUFFER_B));
    }

    TEST_CASE("combine material: passthrough additive nocull onto _rt_default") {
        const auto m = vulkan::buildVolumetricCombineMaterial();
        CHECK(m.shader == "passthrough");
        CHECK(m.blending == "additive");
        CHECK(m.cullmode == "nocull");
        CHECK(m.depthtest == "disabled");
        CHECK(m.depthwrite == "disabled");
        REQUIRE_FALSE(m.textures.empty());
        CHECK(m.textures[0] == std::string(WE_VOLUMETRICS_LIGHT_BUFFER));
    }
}

TEST_SUITE("SceneToRenderGraph_Volumetrics_QualityIntegration") {
    TEST_CASE("ultra post-processing → quality=4 propagated into VolumetricsConfig") {
        VolumetricSceneFixture f;
        f.scene.resolved_postprocessing = "ultra";
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        CHECK(f.scene.volumetricsConfig.quality == 4u);
    }

    TEST_CASE("low post-processing → quality=1") {
        VolumetricSceneFixture f;
        f.scene.resolved_postprocessing = "low";
        f.buildLights(1);
        vulkan::BuildVolumetricNodes(f.scene);
        CHECK(f.scene.volumetricsConfig.quality == 1u);
    }
}
