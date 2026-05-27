#include <doctest.h>

#include "WPShaderValueUpdater.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneRenderTarget.h"
#include "Scene/SceneShader.h"
#include "SpecTexs.hpp"

#include <Eigen/Dense>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace wallpaper;

namespace
{

struct UniformWrite {
    std::string        name;
    std::vector<float> values;
};

// Capture every (name, ShaderValue) pair the writer emits without holding any
// reference back into the writer's stack-temporary arrays.
struct UniformCapture {
    std::vector<UniformWrite> writes;

    UpdateUniformOp op() {
        return [this](std::string_view name, const ShaderValue& v) {
            UniformWrite w;
            w.name = std::string(name);
            w.values.assign(v.data(), v.data() + v.size());
            writes.push_back(std::move(w));
        };
    }

    const UniformWrite* find(std::string_view name) const {
        for (const auto& w : writes) {
            if (w.name == name) return &w;
        }
        return nullptr;
    }
};

ExistsUniformOp makeExistsOp(std::initializer_list<std::string_view> names) {
    std::unordered_set<std::string> set;
    for (auto n : names) set.emplace(n);
    return [set = std::move(set)](std::string_view n) {
        return set.find(std::string(n)) != set.end();
    };
}

// Minimal SceneNode with mesh+material — UpdateUniforms early-returns when
// either is absent, so a usable test node needs both. The mesh/material
// contents are inert; only the ExistsUniformOp from InitUniforms picks which
// branches the writer runs.
struct NodeFixture {
    std::shared_ptr<SceneNode> node;
    std::shared_ptr<SceneMesh> mesh;

    static NodeFixture make() {
        NodeFixture f;
        f.node = std::make_shared<SceneNode>(
            Eigen::Vector3f::Zero(), Eigen::Vector3f(1, 1, 1), Eigen::Vector3f(0, 0, 0));
        f.mesh = std::make_shared<SceneMesh>();
        SceneMaterial mat;
        mat.name = "test_mat";
        f.mesh->AddMaterial(std::move(mat));
        f.node->AddMesh(f.mesh);
        return f;
    }
};

SceneCamera* installActiveCamera(Scene& scene) {
    auto cam = std::make_shared<SceneCamera>(1920, 1080, 0.01f, 1000.0f);
    cam->SetDirectLookAt(
        Eigen::Vector3d(0, 0, 1), Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 1, 0));
    scene.cameras["__test__"] = cam;
    scene.activeCamera        = cam.get();
    return cam.get();
}

} // namespace

// Hoist render-target name resolution from the per-frame UpdateUniforms hot
// path into the prepare-time InitUniforms seam.  Before this change, every
// pass with RT uniforms allocated a fresh std::string per RT name per frame
// and hashed Scene::renderTargets twice to recover the (width, height,
// mipmap_level) tuple — all data that is parse-time-immutable once the scene
// is built.  The cases below pin the contract:
//
//   1. After InitUniforms, the (w, h, mip) tuple must already be resolved
//      and cached on WPUniformInfo::Tex so the per-frame block reads it
//      directly without re-touching Scene::renderTargets or
//      WPShaderValueData::renderTargets.
//
//   2. The link-tex ("_rt_link_N") → offscreen ("_rt_offscreen_N") rewrite
//      must happen at prepare time too — Apply must not re-run the
//      ParseLinkTex / GenOffscreenRT helpers per frame.
//
//   3. RT misses must propagate as "skip this slot" without uploading
//      anything (matches the pre-change `continue` branch).
//
//   4. Once the (w, h, mip) tuple is cached, mutating
//      WPShaderValueData::renderTargets[*].second AFTER InitUniforms must
//      NOT change Apply's output — the cache decouples the per-frame block
//      from the parse-time name strings.  This is the contract that pins
//      the hoist: if Apply still re-reads the stored name on every frame,
//      this case fails.
//
//   5. The single legal post-parse mutation (CopyPass::prepare toggling
//      SceneRenderTarget::allowReuse) must not invalidate the cached
//      tuple — width / height / mipmap_level are unchanged.
TEST_SUITE("WPShaderValueUpdater RT-name resolve at prepare") {
    TEST_CASE("InitUniforms caches (width, height, mipmap_level) tuple on info.texs") {
        Scene scene;
        installActiveCamera(scene);

        SceneRenderTarget rt;
        rt.width                   = 512;
        rt.height                  = 256;
        rt.mipmap_level            = 3;
        scene.renderTargets["foo"] = rt;

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);

        WPShaderValueData data;
        data.renderTargets.emplace_back(0u, std::string("foo"));
        updater.SetNodeData(f.node.get(), data);

        updater.InitUniforms(
            f.node.get(),
            makeExistsOp({ WE_GLTEX_RESOLUTION_NAMES[0], WE_GLTEX_MIPMAPINFO_NAMES[0] }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* res = cap.find(WE_GLTEX_RESOLUTION_NAMES[0]);
        REQUIRE(res != nullptr);
        REQUIRE(res->values.size() == 4);
        CHECK(res->values[0] == doctest::Approx(512.0f));
        CHECK(res->values[1] == doctest::Approx(256.0f));
        CHECK(res->values[2] == doctest::Approx(512.0f));
        CHECK(res->values[3] == doctest::Approx(256.0f));

        const auto* mip = cap.find(WE_GLTEX_MIPMAPINFO_NAMES[0]);
        REQUIRE(mip != nullptr);
        REQUIRE(mip->values.size() == 1);
        CHECK(mip->values[0] == doctest::Approx(3.0f));
    }

    TEST_CASE("link-tex names resolve to _rt_offscreen_N at prepare time") {
        // The data.renderTargets entry stores "_rt_link_42" but the dimensions
        // live on "_rt_offscreen_42" in Scene::renderTargets.  After RC7's
        // hoist, Apply must read the resolved tuple from info.texs without
        // re-running ParseLinkTex / GenOffscreenRT.
        Scene scene;
        installActiveCamera(scene);

        SceneRenderTarget rt;
        rt.width                                = 128;
        rt.height                               = 64;
        rt.mipmap_level                         = 1;
        scene.renderTargets[GenOffscreenRT(42)] = rt;

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);

        WPShaderValueData data;
        data.renderTargets.emplace_back(1u, GenLinkTex(42));
        updater.SetNodeData(f.node.get(), data);

        updater.InitUniforms(f.node.get(), makeExistsOp({ WE_GLTEX_RESOLUTION_NAMES[1] }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* res = cap.find(WE_GLTEX_RESOLUTION_NAMES[1]);
        REQUIRE(res != nullptr);
        REQUIRE(res->values.size() == 4);
        CHECK(res->values[0] == doctest::Approx(128.0f));
        CHECK(res->values[1] == doctest::Approx(64.0f));
    }

    TEST_CASE("RT miss propagates as no-upload (skip slot)") {
        // Scene::renderTargets is empty for the requested name; the slot must
        // be skipped (no g_TextureN{Resolution,MipMapInfo} write).  Matches
        // the original `if (... .count(rtName) == 0) continue;` branch.
        Scene scene;
        installActiveCamera(scene);

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);

        WPShaderValueData data;
        data.renderTargets.emplace_back(2u, std::string("nonexistent"));
        updater.SetNodeData(f.node.get(), data);

        updater.InitUniforms(
            f.node.get(),
            makeExistsOp({ WE_GLTEX_RESOLUTION_NAMES[2], WE_GLTEX_MIPMAPINFO_NAMES[2] }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        CHECK(cap.find(WE_GLTEX_RESOLUTION_NAMES[2]) == nullptr);
        CHECK(cap.find(WE_GLTEX_MIPMAPINFO_NAMES[2]) == nullptr);
    }

    TEST_CASE("Apply ignores post-prepare mutation of WPShaderValueData::renderTargets name") {
        // This is the load-bearing case: it pins that Apply reads the
        // RESOLVED tuple from info.texs, not the stored RT NAME from
        // WPShaderValueData::renderTargets.  Pre-RC7, Apply re-resolves the
        // name on every frame, so corrupting the name AFTER InitUniforms
        // makes Apply miss → upload nothing.  Post-RC7, the cache decouples
        // Apply from the name, so it still uploads the right (w, h, mip).
        Scene scene;
        installActiveCamera(scene);

        SceneRenderTarget rt;
        rt.width                   = 1024;
        rt.height                  = 768;
        rt.mipmap_level            = 4;
        scene.renderTargets["bar"] = rt;

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);

        WPShaderValueData data;
        data.renderTargets.emplace_back(3u, std::string("bar"));
        updater.SetNodeData(f.node.get(), data);

        updater.InitUniforms(
            f.node.get(),
            makeExistsOp({ WE_GLTEX_RESOLUTION_NAMES[3], WE_GLTEX_MIPMAPINFO_NAMES[3] }));

        // Corrupt the stored name AFTER prepare — drop the entry entirely so
        // the pre-RC7 per-frame loop has nothing to iterate.  Post-RC7, the
        // cache on info.texs[3] still drives the upload from the previously
        // resolved tuple.
        WPShaderValueData broken;
        broken.renderTargets.emplace_back(3u, std::string("does_not_exist"));
        updater.SetNodeData(f.node.get(), broken);

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* res = cap.find(WE_GLTEX_RESOLUTION_NAMES[3]);
        REQUIRE(res != nullptr);
        REQUIRE(res->values.size() == 4);
        CHECK(res->values[0] == doctest::Approx(1024.0f));
        CHECK(res->values[1] == doctest::Approx(768.0f));

        const auto* mip = cap.find(WE_GLTEX_MIPMAPINFO_NAMES[3]);
        REQUIRE(mip != nullptr);
        REQUIRE(mip->values.size() == 1);
        CHECK(mip->values[0] == doctest::Approx(4.0f));
    }

    TEST_CASE("allowReuse toggle does not invalidate the cached tuple") {
        // CopyPass::prepare is the one code path allowed to mutate a
        // SceneRenderTarget post-parse (toggles allowReuse=true on the
        // dst entry).  It never touches width / height / mipmap_level, so
        // the cached tuple must remain valid across the toggle.
        Scene scene;
        installActiveCamera(scene);

        SceneRenderTarget rt;
        rt.width                   = 800;
        rt.height                  = 600;
        rt.mipmap_level            = 2;
        scene.renderTargets["baz"] = rt;

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);

        WPShaderValueData data;
        data.renderTargets.emplace_back(4u, std::string("baz"));
        updater.SetNodeData(f.node.get(), data);

        updater.InitUniforms(f.node.get(), makeExistsOp({ WE_GLTEX_RESOLUTION_NAMES[4] }));

        // Simulate the allowReuse toggle CopyPass::prepare performs.
        scene.renderTargets["baz"].allowReuse = true;

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* res = cap.find(WE_GLTEX_RESOLUTION_NAMES[4]);
        REQUIRE(res != nullptr);
        REQUIRE(res->values.size() == 4);
        CHECK(res->values[0] == doctest::Approx(800.0f));
        CHECK(res->values[1] == doctest::Approx(600.0f));
    }
}
