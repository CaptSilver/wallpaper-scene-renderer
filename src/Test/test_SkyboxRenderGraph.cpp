#include <doctest.h>

#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/SkyboxPass.hpp"
#include "VulkanRender/SkyboxMath.hpp"
#include "VulkanRender/CustomShaderPass.hpp" // SelectOutputLoadOp — the consumer of MarkSkyboxOutputCleared
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneMaterial.h"
#include "SpecTexs.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

using namespace wallpaper;

namespace
{

// Walk the graph in topological order, collecting pass names.  Mirrors the
// helper in test_SceneToRenderGraph_Volumetrics.cpp — kept local because the
// skybox pass is a SkyboxPass (not a CustomShaderPass), so the volumetrics
// findPassByName's dynamic_cast<CustomShaderPass*> wouldn't resolve it.
std::vector<std::string> collectPassNames(rg::RenderGraph& rgraph) {
    std::vector<std::string> names;
    for (auto id : rgraph.topologicalOrder()) {
        auto* pn = rgraph.getPassNode(id);
        if (! pn) continue;
        names.emplace_back(pn->name());
    }
    return names;
}

vulkan::SkyboxPass* findSkyboxPass(rg::RenderGraph& rgraph, std::string_view name) {
    for (auto id : rgraph.topologicalOrder()) {
        auto* pn = rgraph.getPassNode(id);
        if (! pn) continue;
        if (pn->name() != name) continue;
        return dynamic_cast<vulkan::SkyboxPass*>(rgraph.getPass(id));
    }
    return nullptr;
}

// Minimal skybox scene: has_skybox on, one layer id, a resolved panorama key.
struct SkyboxSceneFixture {
    Scene scene;

    void makeSkybox(const std::string& tex_key = "panorama.png", i32 layer_id = 5) {
        scene.has_skybox = true;
        scene.skyboxLayerIds.push_back(layer_id);
        scene.skyboxTexKey = tex_key;
    }

    // Append a plain renderable child to the scene graph so an integrated
    // sceneToRenderGraph() run emits a per-node CustomShaderPass we can order
    // the skybox pass against.  No camera → the node takes the normal
    // CustomShaderPass emission path in ToGraphPass.
    void addRenderableLayer(const std::string& pass_name) {
        auto          node = std::make_shared<SceneNode>();
        auto          mesh = std::make_shared<SceneMesh>();
        SceneMaterial mat;
        mat.name = pass_name;
        mesh->AddMaterial(std::move(mat));
        node->AddMesh(mesh);
        node->ID() = 5;
        scene.sceneGraph->AppendChild(node);
    }
};

} // namespace

TEST_SUITE("SkyboxRenderGraph") {
    TEST_CASE("has_skybox scene emits exactly one pass named skybox") {
        SkyboxSceneFixture f;
        f.makeSkybox();
        rg::RenderGraph rgraph;
        vulkan::emitSkyboxPass(rgraph, f.scene);

        auto names = collectPassNames(rgraph);
        auto count = std::count(names.begin(), names.end(), std::string("skybox"));
        CHECK(count == 1);
    }

    TEST_CASE("skybox pass writes _rt_default and reads the panorama tex key") {
        SkyboxSceneFixture f;
        f.makeSkybox("myPano.png");
        rg::RenderGraph rgraph;
        vulkan::emitSkyboxPass(rgraph, f.scene);

        auto* pass = findSkyboxPass(rgraph, "skybox");
        REQUIRE(pass != nullptr);
        CHECK(pass->desc().output == std::string(SpecTex_Default));
        CHECK(pass->desc().pano_tex_key == "myPano.png");
    }

    TEST_CASE("has_skybox false -> no skybox pass emitted") {
        SkyboxSceneFixture f;
        // Leave has_skybox default (false) but still set a tex key + id — the
        // gate must hinge on has_skybox, not on the ancillary state.
        f.scene.skyboxLayerIds.push_back(9);
        f.scene.skyboxTexKey = "panorama.png";
        rg::RenderGraph rgraph;
        vulkan::emitSkyboxPass(rgraph, f.scene);

        CHECK(collectPassNames(rgraph).empty());
    }

    TEST_CASE("empty skyboxLayerIds despite has_skybox -> no pass") {
        SkyboxSceneFixture f;
        f.scene.has_skybox   = true;
        f.scene.skyboxTexKey = "panorama.png";
        // skyboxLayerIds intentionally empty.
        rg::RenderGraph rgraph;
        vulkan::emitSkyboxPass(rgraph, f.scene);

        CHECK(collectPassNames(rgraph).empty());
    }

    TEST_CASE("empty skyboxTexKey despite has_skybox + layer id -> no pass") {
        SkyboxSceneFixture f;
        f.scene.has_skybox = true;
        f.scene.skyboxLayerIds.push_back(3);
        // skyboxTexKey intentionally empty — nothing to sample.
        rg::RenderGraph rgraph;
        vulkan::emitSkyboxPass(rgraph, f.scene);

        CHECK(collectPassNames(rgraph).empty());
    }

    TEST_CASE("skybox pass ordered before per-node layer passes") {
        SkyboxSceneFixture f;
        f.makeSkybox("panorama.png");
        f.addRenderableLayer("layer_mat");

        auto rgraph = wallpaper::sceneToRenderGraph(f.scene);
        auto names  = collectPassNames(*rgraph);

        auto pos = [&names](const std::string& n) {
            return std::find(names.begin(), names.end(), n) - names.begin();
        };
        const auto skybox_pos = pos("skybox");
        const auto layer_pos  = pos("layer_mat");
        REQUIRE(skybox_pos != (long)names.size()); // skybox present
        REQUIRE(layer_pos != (long)names.size());  // layer present
        CHECK(skybox_pos < layer_pos);
    }

    TEST_CASE("null activeCamera -> identity invViewProj, emit/topology does not crash") {
        SkyboxSceneFixture f;
        f.makeSkybox("panorama.png");
        REQUIRE(f.scene.activeCamera == nullptr);

        // Both the standalone emit and the integrated build must tolerate a
        // scene with no active camera (Increment A uses identity invViewProj).
        rg::RenderGraph rgraph;
        vulkan::emitSkyboxPass(rgraph, f.scene);
        auto emit_names = collectPassNames(rgraph);
        CHECK(std::count(emit_names.begin(), emit_names.end(), std::string("skybox")) == 1);

        auto integrated = wallpaper::sceneToRenderGraph(f.scene);
        auto names      = collectPassNames(*integrated);
        CHECK(std::count(names.begin(), names.end(), std::string("skybox")) == 1);
    }
}

TEST_SUITE("SkyboxRenderGraph_EquirectUV") {
    // Mirrors the fragment-shader mapping:
    //   u = atan2(dir.x, dir.z)/(2π) + 0.5
    //   v = acos(clamp(dir.y,-1,1))/π
    TEST_CASE("+Z maps to the panorama center (0.5, 0.5)") {
        auto uv = vulkan::equirectUV(Eigen::Vector3f { 0.0f, 0.0f, 1.0f });
        CHECK(uv.x() == doctest::Approx(0.5f));
        CHECK(uv.y() == doctest::Approx(0.5f));
    }

    TEST_CASE("+Y maps to the top of the panorama (v approx 0)") {
        auto uv = vulkan::equirectUV(Eigen::Vector3f { 0.0f, 1.0f, 0.0f });
        CHECK(uv.y() == doctest::Approx(0.0f));
    }

    TEST_CASE("-Y maps to the bottom of the panorama (v approx 1)") {
        auto uv = vulkan::equirectUV(Eigen::Vector3f { 0.0f, -1.0f, 0.0f });
        CHECK(uv.y() == doctest::Approx(1.0f));
    }

    TEST_CASE("+X maps to u = 0.75 (a quarter turn from +Z toward +X)") {
        auto uv = vulkan::equirectUV(Eigen::Vector3f { 1.0f, 0.0f, 0.0f });
        CHECK(uv.x() == doctest::Approx(0.75f));
        CHECK(uv.y() == doctest::Approx(0.5f));
    }

    TEST_CASE("-X maps to u = 0.25 (a quarter turn from +Z toward -X)") {
        auto uv = vulkan::equirectUV(Eigen::Vector3f { -1.0f, 0.0f, 0.0f });
        CHECK(uv.x() == doctest::Approx(0.25f));
        CHECK(uv.y() == doctest::Approx(0.5f));
    }

    TEST_CASE("out-of-range dir.y clamps (does not NaN)") {
        // A non-normalized dir with |y| > 1 must clamp inside acos's domain.
        auto uv = vulkan::equirectUV(Eigen::Vector3f { 0.0f, 5.0f, 0.0f });
        CHECK(std::isfinite(uv.y()));
        CHECK(uv.y() == doctest::Approx(0.0f));
    }
}

TEST_SUITE("SkyboxRenderGraph_ForegroundPredicate") {
    TEST_CASE("no skybox -> predicate false regardless of node count") {
        Scene s;
        CHECK(vulkan::skyboxHasForegroundNodes(s) == false);
    }

    TEST_CASE("skybox with only its own layer -> no foreground -> false") {
        Scene s;
        s.has_skybox = true;
        s.skyboxLayerIds.push_back(1);
        // One renderable node, which IS the skybox layer.
        auto node = std::make_shared<SceneNode>();
        auto mesh = std::make_shared<SceneMesh>();
        mesh->AddMaterial(SceneMaterial {});
        node->AddMesh(mesh);
        node->ID() = 1;
        s.sceneGraph->AppendChild(node);

        CHECK(vulkan::skyboxHasForegroundNodes(s) == false);
    }

    TEST_CASE("skybox plus extra renderable nodes -> foreground -> true") {
        Scene s;
        s.has_skybox = true;
        s.skyboxLayerIds.push_back(1);
        for (i32 id : { 1, 2, 3 }) {
            auto node = std::make_shared<SceneNode>();
            auto mesh = std::make_shared<SceneMesh>();
            mesh->AddMaterial(SceneMaterial {});
            node->AddMesh(mesh);
            node->ID() = id;
            s.sceneGraph->AppendChild(node);
        }
        // 3 renderable nodes, 1 of which is the skybox → 2 foreground.
        CHECK(vulkan::skyboxHasForegroundNodes(s) == true);
    }
}

// The skybox pass fills its whole output every frame, making it the RT's base
// write.  CustomShaderPass picks CLEAR for the first writer of an RT that is
// not in Scene::clearedRTs (SelectOutputLoadOp) — so if the skybox does not
// register its output, the scene's first flat layer re-clears _rt_default to
// clearColor ON TOP of the freshly drawn panorama and the background is lost.
// Found on-GPU (RADV + lavapipe agreed): screen showed exactly clearColor.
TEST_SUITE("SkyboxPass_ClearedRTs") {
    TEST_CASE("skybox base write marks its RT cleared -> next writer LOADs") {
        Scene s;
        vulkan::MarkSkyboxOutputCleared(s, std::string(SpecTex_Default));
        const bool already = s.clearedRTs.count(std::string(SpecTex_Default)) != 0;
        CHECK(already);
        CHECK(vulkan::SelectOutputLoadOp(false, already) == VK_ATTACHMENT_LOAD_OP_LOAD);
    }

    TEST_CASE("no skybox mark -> first writer still clears (flat-scene base path)") {
        Scene      s;
        const bool already = s.clearedRTs.count(std::string(SpecTex_Default)) != 0;
        CHECK_FALSE(already);
        CHECK(vulkan::SelectOutputLoadOp(false, already) == VK_ATTACHMENT_LOAD_OP_CLEAR);
    }

    TEST_CASE("a forced clear still wins over the skybox mark (compose base pass)") {
        Scene s;
        vulkan::MarkSkyboxOutputCleared(s, std::string(SpecTex_Default));
        const bool already = s.clearedRTs.count(std::string(SpecTex_Default)) != 0;
        CHECK(vulkan::SelectOutputLoadOp(true, already) == VK_ATTACHMENT_LOAD_OP_CLEAR);
    }
}

// The skybox UBO lives in the frames-in-flight dyn_buf: recordUpload re-copies
// the CURRENT slot's whole staging every frame, so a value written once at
// prepare() only survives on frames whose slot saw the write — on the others
// the shader reads zeros, farPoint*0 collapses to a NaN direction, and the
// panorama samples black.  Found on-GPU (RADV + lavapipe both black).  The
// contract is CustomShaderPass's: dynamic uniforms are rewritten every frame
// during execute(); MakeSkyboxUbo is that per-frame payload.
TEST_SUITE("SkyboxPass_Ubo") {
    TEST_CASE("std140 block is 80 bytes: mat4 + yaw + 3-float pad") {
        CHECK(sizeof(vulkan::SkyboxUbo) == 80);
    }

    TEST_CASE("Increment A payload is the identity matrix with zero yaw") {
        const auto ubo = vulkan::MakeSkyboxUbo(0.0f);
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) CHECK(ubo.invViewProj[c * 4 + r] == (c == r ? 1.0f : 0.0f));
        CHECK(ubo.yawRad == 0.0f);
        CHECK(ubo.pad[0] == 0.0f);
        CHECK(ubo.pad[1] == 0.0f);
        CHECK(ubo.pad[2] == 0.0f);
    }

    TEST_CASE("yaw passes through") {
        const auto ubo = vulkan::MakeSkyboxUbo(1.5f);
        CHECK(ubo.yawRad == 1.5f);
    }
}

// MSAA scenes render layers into a 4x color buffer and RESOLVE it over
// _rt_default at the end of every layer pass — a fullscreen overwrite that
// erases the skybox background no matter what the skybox drew (and once the
// first layer LOADs instead of clearing, the MSAA buffer starts as
// uninitialized memory).  Until the skybox participates in the MSAA chain,
// skybox scenes render single-sampled.  Found on-GPU: the "background" was
// the panorama's staging bytes leaking through the uninitialized MSAA buffer.
TEST_SUITE("Skybox_MsaaPolicy") {
    TEST_CASE("skybox scene forces 1spp") {
        CHECK(Scene::skyboxMsaaSamples(true, 4) == 1);
        CHECK(Scene::skyboxMsaaSamples(true, 8) == 1);
        CHECK(Scene::skyboxMsaaSamples(true, 1) == 1);
    }

    TEST_CASE("non-skybox scene keeps the requested sample count") {
        CHECK(Scene::skyboxMsaaSamples(false, 4) == 4);
        CHECK(Scene::skyboxMsaaSamples(false, 1) == 1);
    }
}
