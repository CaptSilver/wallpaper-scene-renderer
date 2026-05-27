#include <doctest.h>

#include "WPShaderValueUpdater.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneLight.hpp"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneRenderTarget.h"
#include "Scene/SceneShader.h"
#include "SpecTexs.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

using namespace wallpaper;

namespace
{
// Replay the WPShaderValueUpdater interface contract: each updateOp() call
// arrives with a uniform name + ShaderValue.  We snapshot enough of the
// value into POD form so assertions can run after the writer returns
// without keeping any reference alive into the writer's stack-temporary
// std::array temporaries.
struct UniformWrite {
    std::string        name;
    std::vector<float> values;
};

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

// Build an ExistsUniformOp from a fixed allow-list of uniform names.  Used
// in InitUniforms so the WPUniformInfo flags reflect exactly the uniforms
// the test cares about.
ExistsUniformOp makeExistsOp(std::initializer_list<std::string_view> names) {
    std::unordered_set<std::string> set;
    for (auto n : names) set.emplace(n);
    return [set = std::move(set)](std::string_view n) {
        return set.find(std::string(n)) != set.end();
    };
}

// Minimal node-with-mesh-and-material fixture.  WPShaderValueUpdater bails
// out of UpdateUniforms() early when Mesh() or Material() is null, so a
// usable test node MUST have both.  The mesh/material's shader contents are
// irrelevant to uniform dispatch — only the ExistsUniformOp from
// InitUniforms drives which branches the writer runs.
struct NodeFixture {
    std::shared_ptr<SceneNode> node;
    std::shared_ptr<SceneMesh> mesh;

    static NodeFixture make(const Eigen::Vector3f& translate = Eigen::Vector3f::Zero()) {
        NodeFixture f;
        f.node = std::make_shared<SceneNode>(translate,
                                             Eigen::Vector3f(1, 1, 1),
                                             Eigen::Vector3f(0, 0, 0));
        f.mesh = std::make_shared<SceneMesh>();
        SceneMaterial mat;
        mat.name = "test_mat";
        f.mesh->AddMaterial(std::move(mat));
        f.node->AddMesh(f.mesh);
        return f;
    }
};

// Install a no-op ortho camera so the matrix-recompute branch in
// UpdateUniforms() can run.  Camera is owned by `scene.cameras` so the
// raw pointer assigned to `activeCamera` stays alive.
SceneCamera* installActiveCamera(Scene& scene, double w = 1920.0, double h = 1080.0) {
    auto cam = std::make_shared<SceneCamera>((i32)w, (i32)h, 0.01f, 1000.0f);
    cam->SetDirectLookAt(Eigen::Vector3d(0, 0, 1),
                         Eigen::Vector3d(0, 0, 0),
                         Eigen::Vector3d(0, 1, 0));
    scene.cameras["__test__"] = cam;
    scene.activeCamera        = cam.get();
    return cam.get();
}
} // namespace

// =============================================================================
//   g_Time / g_DayTime / g_PointerPosition / g_TexelSize{Half} / g_Screen
//   — scalar + vec2/3 dispatch from updater-owned state into the writer.
// =============================================================================
TEST_SUITE("WPShaderValueUpdater::Uniforms::time_and_screen") {
    TEST_CASE("g_Time uploads Scene::elapsingTime as scalar float") {
        Scene scene;
        installActiveCamera(scene);
        scene.elapsingTime = 7.5;

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_TIME }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* w = cap.find(G_TIME);
        REQUIRE(w != nullptr);
        REQUIRE(w->values.size() == 1);
        CHECK(w->values[0] == doctest::Approx(7.5f));
    }

    TEST_CASE("g_DayTime defaults to 0 when no time-of-day source is set") {
        // WPShaderValueUpdater's m_dayTime defaults to 0 and is only ever
        // recomputed via a commented-out wall-clock block; in the shipped
        // build it stays at 0.  Pin that behaviour: the writer always
        // pushes the current m_dayTime as a single scalar.
        Scene scene;
        installActiveCamera(scene);

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_DAYTIME }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* w = cap.find(G_DAYTIME);
        REQUIRE(w != nullptr);
        REQUIRE(w->values.size() == 1);
        CHECK(w->values[0] == doctest::Approx(0.0f));
    }

    TEST_CASE("g_PointerPosition forwards interpolated mouse position") {
        // FrameBegin() interpolates m_mousePos toward m_mousePosInput by
        // (frameTime / parallax.delay).  With frameTime == delay the
        // interpolation completes in one frame so the captured value
        // matches the input cleanly.
        Scene scene;
        installActiveCamera(scene);
        scene.frameTime = 0.5;

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_POINTERPOSITION }));
        updater.MouseInput(0.25, 0.75); // populate m_mousePosInput
        updater.FrameBegin();           // lerp m_mousePos toward input

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* w = cap.find(G_POINTERPOSITION);
        REQUIRE(w != nullptr);
        REQUIRE(w->values.size() == 2);
        // Default parallax.delay==0.5; frameTime==delay so lerp t==1.0 and the
        // value lands exactly at the input.
        CHECK(w->values[0] == doctest::Approx(0.25f));
        CHECK(w->values[1] == doctest::Approx(0.75f));
    }

    TEST_CASE("g_TexelSize routes SetTexelSize(x,y) verbatim") {
        Scene scene;
        installActiveCamera(scene);

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.SetTexelSize(1.0f / 800.0f, 1.0f / 600.0f);
        updater.InitUniforms(f.node.get(),
                             makeExistsOp({ G_TEXELSIZE, G_TEXELSIZEHALF }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* w = cap.find(G_TEXELSIZE);
        REQUIRE(w != nullptr);
        REQUIRE(w->values.size() == 2);
        CHECK(w->values[0] == doctest::Approx(1.0f / 800.0f));
        CHECK(w->values[1] == doctest::Approx(1.0f / 600.0f));

        // g_TexelSizeHalf is the same pair / 2.
        const auto* h = cap.find(G_TEXELSIZEHALF);
        REQUIRE(h != nullptr);
        REQUIRE(h->values.size() == 2);
        CHECK(h->values[0] == doctest::Approx(0.5f / 800.0f));
        CHECK(h->values[1] == doctest::Approx(0.5f / 600.0f));
    }

    TEST_CASE("g_Screen packs (w, h, w/h aspect)") {
        Scene scene;
        installActiveCamera(scene);

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.SetScreenSize(1920, 1080);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_SCREEN }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* w = cap.find(G_SCREEN);
        REQUIRE(w != nullptr);
        REQUIRE(w->values.size() == 3);
        CHECK(w->values[0] == doctest::Approx(1920.0f));
        CHECK(w->values[1] == doctest::Approx(1080.0f));
        CHECK(w->values[2] == doctest::Approx(1920.0f / 1080.0f));
    }
}

// =============================================================================
//   g_ParallaxPosition — depth-parallax mouse uniform with Y-flip + center
//   offset scaled by parallax.mouseinfluence (regression-prone math).
// =============================================================================
TEST_SUITE("WPShaderValueUpdater::Uniforms::parallax_position") {
    TEST_CASE("mouse at screen center maps to (0.5, 0.5) regardless of influence") {
        Scene scene;
        installActiveCamera(scene);

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        WPCameraParallax p;
        p.enable         = true;
        p.amount         = 0.05f;
        p.mouseinfluence = 0.6f;
        updater.SetCameraParallax(p);

        updater.InitUniforms(f.node.get(), makeExistsOp({ G_PARALLAXPOSITION }));

        // Default m_mousePos == { 0.5f, 0.5f } when no MouseInput has run.
        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* w = cap.find(G_PARALLAXPOSITION);
        REQUIRE(w != nullptr);
        REQUIRE(w->values.size() == 2);
        CHECK(w->values[0] == doctest::Approx(0.5f));
        CHECK(w->values[1] == doctest::Approx(0.5f));
    }

    TEST_CASE("y is flipped via (0.5 - mouseY), offset by mouseinfluence") {
        // m_mousePos.x = 0.7 -> centered = (0.7 - 0.5) = 0.2
        // m_mousePos.y = 0.2 -> centered = (0.5 - 0.2) = 0.3 (Y-flip)
        // para = 0.5 + centered * mouseinfluence(=1)
        //      = (0.5 + 0.2, 0.5 + 0.3) = (0.7, 0.8)
        Scene scene;
        installActiveCamera(scene);
        scene.frameTime = 1.0; // force one-shot lerp completion

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        WPCameraParallax p;
        p.enable         = true;
        p.mouseinfluence = 1.0f;
        p.delay          = 0.5f;
        updater.SetCameraParallax(p);

        updater.InitUniforms(f.node.get(), makeExistsOp({ G_PARALLAXPOSITION }));
        updater.MouseInput(0.7, 0.2);
        updater.FrameBegin();

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* w = cap.find(G_PARALLAXPOSITION);
        REQUIRE(w != nullptr);
        REQUIRE(w->values.size() == 2);
        CHECK(w->values[0] == doctest::Approx(0.7f));
        CHECK(w->values[1] == doctest::Approx(0.8f));
    }
}

// =============================================================================
//   g_LightAmbientColor / g_LightSkylightColor — scene-level color forwarders.
// =============================================================================
TEST_SUITE("WPShaderValueUpdater::Uniforms::ambient_and_skylight") {
    TEST_CASE("g_LightAmbientColor forwards Scene::ambientColor as float3") {
        Scene scene;
        installActiveCamera(scene);
        scene.ambientColor = { 0.10f, 0.20f, 0.30f };

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_LIGHTAMBIENTCOLOR }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* w = cap.find(G_LIGHTAMBIENTCOLOR);
        REQUIRE(w != nullptr);
        REQUIRE(w->values.size() == 3);
        CHECK(w->values[0] == doctest::Approx(0.10f));
        CHECK(w->values[1] == doctest::Approx(0.20f));
        CHECK(w->values[2] == doctest::Approx(0.30f));
    }

    TEST_CASE("g_LightSkylightColor forwards Scene::skylightColor as float3") {
        Scene scene;
        installActiveCamera(scene);
        scene.skylightColor = { 0.75f, 0.50f, 0.25f };

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_LIGHTSKYLIGHTCOLOR }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* w = cap.find(G_LIGHTSKYLIGHTCOLOR);
        REQUIRE(w != nullptr);
        REQUIRE(w->values.size() == 3);
        CHECK(w->values[0] == doctest::Approx(0.75f));
        CHECK(w->values[1] == doctest::Approx(0.50f));
        CHECK(w->values[2] == doctest::Approx(0.25f));
    }

    TEST_CASE("g_LightAmbientColor is NOT uploaded when uniform is absent") {
        // Negative case: even when ambientColor is set, the writer must skip
        // the upload if InitUniforms saw no has_LIGHTAMBIENTCOLOR.  Ensures
        // the per-node uniform existence flag actually gates the write.
        Scene scene;
        installActiveCamera(scene);
        scene.ambientColor = { 0.5f, 0.5f, 0.5f };

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ /* nothing */ }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        CHECK(cap.find(G_LIGHTAMBIENTCOLOR) == nullptr);
    }
}

// =============================================================================
//   g_LightsPosition / g_LightsColorRadius / g_LightsColorPremultiplied
//   — the 4-light fixed-size uniform table.  Includes the reflected camera
//   Y-mirror branch.
// =============================================================================
TEST_SUITE("WPShaderValueUpdater::Uniforms::point_lights") {
    TEST_CASE("packs (world.xyz, exponent) into g_LightsPosition") {
        Scene scene;
        installActiveCamera(scene);

        // One point light at world (10, 20, 30) with exponent 0.5.
        auto light = std::make_unique<SceneLight>(Eigen::Vector3f(1, 1, 1),
                                                  100.0f, 2.0f, 0.5f);
        auto light_node = std::make_shared<SceneNode>(Eigen::Vector3f(10, 20, 30),
                                                      Eigen::Vector3f(1, 1, 1),
                                                      Eigen::Vector3f(0, 0, 0));
        light->setNode(light_node);
        scene.sceneGraph->AppendChild(light_node);
        scene.lights.push_back(std::move(light));

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_LP, G_LCR }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* lp = cap.find(G_LP);
        REQUIRE(lp != nullptr);
        REQUIRE(lp->values.size() == 16); // 4 lights * vec4
        CHECK(lp->values[0] == doctest::Approx(10.0f));
        CHECK(lp->values[1] == doctest::Approx(20.0f));
        CHECK(lp->values[2] == doctest::Approx(30.0f));
        CHECK(lp->values[3] == doctest::Approx(0.5f)); // exponent in .w
        // Lights 1..3 are zero-filled.
        for (size_t i = 4; i < 16; ++i) {
            CHECK(lp->values[i] == doctest::Approx(0.0f));
        }
    }

    TEST_CASE("packs (color*intensity, radius) into g_LightsColorRadius") {
        Scene scene;
        installActiveCamera(scene);

        auto light = std::make_unique<SceneLight>(Eigen::Vector3f(1.0f, 0.5f, 0.25f),
                                                  /*radius*/ 250.0f,
                                                  /*intensity*/ 4.0f);
        auto light_node = std::make_shared<SceneNode>(Eigen::Vector3f::Zero(),
                                                      Eigen::Vector3f(1, 1, 1),
                                                      Eigen::Vector3f(0, 0, 0));
        light->setNode(light_node);
        scene.sceneGraph->AppendChild(light_node);
        scene.lights.push_back(std::move(light));

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_LP, G_LCR }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* lcr = cap.find(G_LCR);
        REQUIRE(lcr != nullptr);
        REQUIRE(lcr->values.size() == 16);
        // colorIntensity() = color * intensity
        CHECK(lcr->values[0] == doctest::Approx(1.0f * 4.0f));
        CHECK(lcr->values[1] == doctest::Approx(0.5f * 4.0f));
        CHECK(lcr->values[2] == doctest::Approx(0.25f * 4.0f));
        CHECK(lcr->values[3] == doctest::Approx(250.0f)); // radius in .w
    }

    TEST_CASE("g_LightsColorPremultiplied uses color * intensity * radius^2") {
        Scene scene;
        installActiveCamera(scene);

        auto light = std::make_unique<SceneLight>(Eigen::Vector3f(0.1f, 0.2f, 0.4f),
                                                  /*radius*/ 10.0f,
                                                  /*intensity*/ 3.0f);
        auto light_node = std::make_shared<SceneNode>(Eigen::Vector3f::Zero(),
                                                      Eigen::Vector3f(1, 1, 1),
                                                      Eigen::Vector3f(0, 0, 0));
        light->setNode(light_node);
        scene.sceneGraph->AppendChild(light_node);
        scene.lights.push_back(std::move(light));

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_LP, G_LCR }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        // Premultiplied colour is uploaded under the LP/LCR gate as well
        // (no separate has_LCP bit — the writer always emits it when LP/LCR
        // are present).
        const auto* lcp = cap.find(G_LCP);
        REQUIRE(lcp != nullptr);
        REQUIRE(lcp->values.size() == 12); // 3 lights * vec4 in first three slots
        const float k = 3.0f * 10.0f * 10.0f; // intensity * radius^2
        CHECK(lcp->values[0] == doctest::Approx(0.1f * k));
        CHECK(lcp->values[1] == doctest::Approx(0.2f * k));
        CHECK(lcp->values[2] == doctest::Approx(0.4f * k));
    }

    TEST_CASE("reflected_perspective camera mirrors light Y across floor plane") {
        Scene scene;
        // Install global camera AND reflected camera — the writer reads
        // cameras.at("reflected_perspective") when the node binds it.
        installActiveCamera(scene);
        auto refl = std::make_shared<SceneCamera>(1920, 1080, 0.01f, 1000.0f);
        refl->SetDirectLookAt(Eigen::Vector3d(0, 0, 1),
                              Eigen::Vector3d(0, 0, 0),
                              Eigen::Vector3d(0, 1, 0));
        scene.cameras["reflected_perspective"] = refl;

        // One point light at y = +50 (above floor).
        auto light = std::make_unique<SceneLight>(Eigen::Vector3f(1, 1, 1),
                                                  100.0f, 1.0f);
        auto light_node = std::make_shared<SceneNode>(Eigen::Vector3f(0, 50, 0),
                                                      Eigen::Vector3f(1, 1, 1),
                                                      Eigen::Vector3f(0, 0, 0));
        light->setNode(light_node);
        scene.sceneGraph->AppendChild(light_node);
        scene.lights.push_back(std::move(light));

        auto f = NodeFixture::make();
        f.node->SetCamera("reflected_perspective");
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_LP, G_LCR }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* lp = cap.find(G_LP);
        REQUIRE(lp != nullptr);
        // Light y should be flipped to -50 for reflected lighting.
        CHECK(lp->values[0] == doctest::Approx(0.0f));
        CHECK(lp->values[1] == doctest::Approx(-50.0f));
        CHECK(lp->values[2] == doctest::Approx(0.0f));
    }

    TEST_CASE("caps light uploads at 4 even with more in scene") {
        Scene scene;
        installActiveCamera(scene);

        // Push 6 lights — writer should only consume the first 4.
        for (int i = 0; i < 6; ++i) {
            auto l = std::make_unique<SceneLight>(Eigen::Vector3f(1, 1, 1),
                                                  10.0f, 1.0f);
            auto n = std::make_shared<SceneNode>(Eigen::Vector3f((float)i, 0, 0),
                                                 Eigen::Vector3f(1, 1, 1),
                                                 Eigen::Vector3f(0, 0, 0));
            l->setNode(n);
            scene.sceneGraph->AppendChild(n);
            scene.lights.push_back(std::move(l));
        }

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_LP, G_LCR }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* lp = cap.find(G_LP);
        REQUIRE(lp != nullptr);
        REQUIRE(lp->values.size() == 16);
        // Only lights 0..3 wrote x = 0,1,2,3.
        CHECK(lp->values[0]  == doctest::Approx(0.0f));
        CHECK(lp->values[4]  == doctest::Approx(1.0f));
        CHECK(lp->values[8]  == doctest::Approx(2.0f));
        CHECK(lp->values[12] == doctest::Approx(3.0f));
        // (lights 4/5 not present in the 16 floats; just confirming no
        // overflow occurred.)
    }
}

// =============================================================================
//   g_TextureNResolution / g_TextureNMipMapInfo — render-target size lookup
//   driven by WPShaderValueData::renderTargets entries (index + name pair).
// =============================================================================
TEST_SUITE("WPShaderValueUpdater::Uniforms::rt_size_lookup") {
    TEST_CASE("g_TextureNResolution upload pulls width/height from named RT") {
        Scene scene;
        installActiveCamera(scene);

        SceneRenderTarget rt;
        rt.width        = 800;
        rt.height       = 450;
        rt.mipmap_level = 3;
        scene.renderTargets["custom_rt"] = rt;

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);

        // Production ordering: SetNodeData (parse time) precedes
        // InitUniforms (prepare time) so the prepare-time RT-name
        // resolution sees the populated nodeData.
        WPShaderValueData data;
        data.renderTargets.emplace_back(2u, std::string("custom_rt"));
        updater.SetNodeData(f.node.get(), data);

        updater.InitUniforms(f.node.get(),
                             makeExistsOp({ WE_GLTEX_RESOLUTION_NAMES[2],
                                            WE_GLTEX_MIPMAPINFO_NAMES[2] }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* res = cap.find(WE_GLTEX_RESOLUTION_NAMES[2]);
        REQUIRE(res != nullptr);
        // Layout: (width, height, width, height) — w/h packed twice so the
        // shader can sample either pair (used as both pixel-size and inverse).
        REQUIRE(res->values.size() == 4);
        CHECK(res->values[0] == doctest::Approx(800.0f));
        CHECK(res->values[1] == doctest::Approx(450.0f));
        CHECK(res->values[2] == doctest::Approx(800.0f));
        CHECK(res->values[3] == doctest::Approx(450.0f));

        const auto* mip = cap.find(WE_GLTEX_MIPMAPINFO_NAMES[2]);
        REQUIRE(mip != nullptr);
        REQUIRE(mip->values.size() == 1);
        CHECK(mip->values[0] == doctest::Approx(3.0f));
    }

    TEST_CASE("resolves _rt_link_<id> reference through GenOffscreenRT()") {
        // The link-tex convention "_rt_link_N" resolves to "_rt_offscreen_N"
        // for the dimension lookup (the layer's offscreen RT carries the
        // size, the link is a per-pass texture alias).
        Scene scene;
        installActiveCamera(scene);

        SceneRenderTarget rt;
        rt.width  = 320;
        rt.height = 240;
        scene.renderTargets[GenOffscreenRT(42)] = rt;

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);

        WPShaderValueData data;
        data.renderTargets.emplace_back(0u, GenLinkTex(42));
        updater.SetNodeData(f.node.get(), data);

        updater.InitUniforms(f.node.get(),
                             makeExistsOp({ WE_GLTEX_RESOLUTION_NAMES[0] }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* res = cap.find(WE_GLTEX_RESOLUTION_NAMES[0]);
        REQUIRE(res != nullptr);
        REQUIRE(res->values.size() == 4);
        CHECK(res->values[0] == doctest::Approx(320.0f));
        CHECK(res->values[1] == doctest::Approx(240.0f));
    }

    TEST_CASE("missing RT name skips upload silently") {
        // Author-error robustness: the writer logs once and continues; it
        // must not push a write for the absent RT.
        Scene scene;
        installActiveCamera(scene);

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(),
                             makeExistsOp({ WE_GLTEX_RESOLUTION_NAMES[1] }));

        WPShaderValueData data;
        data.renderTargets.emplace_back(1u, std::string("missing_rt"));
        updater.SetNodeData(f.node.get(), data);

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        CHECK(cap.find(WE_GLTEX_RESOLUTION_NAMES[1]) == nullptr);
    }
}

// =============================================================================
//   Audio spectrum — the volumetric test covers the gate predicate; here we
//   pin the per-frame upload behaviour: with no analyzer, zero-fill all six
//   bands that the WPUniformInfo says exist.
// =============================================================================
TEST_SUITE("WPShaderValueUpdater::Uniforms::audio_zerofill") {
    TEST_CASE("absent analyzer zero-fills all 6 spectrum uniforms") {
        Scene scene;
        installActiveCamera(scene);

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(),
                             makeExistsOp({ G_AUDIOSPECTRUM16LEFT,
                                            G_AUDIOSPECTRUM16RIGHT,
                                            G_AUDIOSPECTRUM32LEFT,
                                            G_AUDIOSPECTRUM32RIGHT,
                                            G_AUDIOSPECTRUM64LEFT,
                                            G_AUDIOSPECTRUM64RIGHT }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        // The writer's zero-fill path uses a single 64-float zeros buffer
        // for every band.  Verify all six are present and start with zero;
        // size depends on ShaderValue clamping into ShaderValueInter (16),
        // so we don't pin a strict length — we only check the contract that
        // every captured float is zero.
        for (auto name : {
                 G_AUDIOSPECTRUM16LEFT, G_AUDIOSPECTRUM16RIGHT,
                 G_AUDIOSPECTRUM32LEFT, G_AUDIOSPECTRUM32RIGHT,
                 G_AUDIOSPECTRUM64LEFT, G_AUDIOSPECTRUM64RIGHT }) {
            const auto* w = cap.find(name);
            REQUIRE_MESSAGE(w != nullptr, "missing band: ", std::string(name));
            CHECK_FALSE(w->values.empty());
            for (float v : w->values) CHECK(v == doctest::Approx(0.0f));
        }
    }
}

// =============================================================================
//   Model / VP / MVP matrix uniforms — confirm the writer dispatches the
//   gating set and that the cache short-circuits the second frame.
// =============================================================================
TEST_SUITE("WPShaderValueUpdater::Uniforms::matrix_block") {
    TEST_CASE("g_ModelViewProjectionMatrix uploads 16 floats on first frame") {
        Scene scene;
        installActiveCamera(scene);

        auto f = NodeFixture::make(Eigen::Vector3f(5, 0, 0));
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_M, G_MVP, G_VP }));

        UniformCapture cap;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap.op());

        const auto* m   = cap.find(G_M);
        const auto* mvp = cap.find(G_MVP);
        const auto* vp  = cap.find(G_VP);
        REQUIRE(m != nullptr);
        REQUIRE(mvp != nullptr);
        REQUIRE(vp != nullptr);
        // ShaderValue::fromMatrix(Matrix4d) packs 16 floats column-major.
        CHECK(m->values.size() == 16);
        CHECK(mvp->values.size() == 16);
        CHECK(vp->values.size() == 16);
        // Model matrix encodes the translate(5,0,0) — for a row-major dump
        // ShaderValue would land 5.0 at index 12; eigen MatrixXd is
        // column-major, so the translate column is (m[12], m[13], m[14]).
        CHECK(m->values[12] == doctest::Approx(5.0f));
    }

    TEST_CASE("static node + static camera reuses cached matrices on frame 2") {
        Scene scene;
        installActiveCamera(scene);

        auto f = NodeFixture::make(Eigen::Vector3f(3, 7, 0));
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_M, G_MVP, G_VP }));

        UniformCapture cap1;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap1.op());
        REQUIRE(cap1.find(G_M) != nullptr);
        auto m1 = cap1.find(G_M)->values;

        // Second frame: nothing moved, gate must serve from cache.  The
        // upload still happens (re-pushed cached value), so the second
        // capture matches the first byte-for-byte.
        UniformCapture cap2;
        updater.UpdateUniforms(f.node.get(), sprites, cap2.op());
        REQUIRE(cap2.find(G_M) != nullptr);
        const auto& m2 = cap2.find(G_M)->values;
        REQUIRE(m1.size() == m2.size());
        for (size_t i = 0; i < m1.size(); ++i) {
            CHECK(m1[i] == doctest::Approx(m2[i]));
        }
    }

    TEST_CASE("moving the node bumps TransEpoch, recomputed translate appears") {
        Scene scene;
        installActiveCamera(scene);

        auto f = NodeFixture::make(Eigen::Vector3f(1, 0, 0));
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_M }));

        UniformCapture cap1;
        sprite_map_t   sprites;
        updater.UpdateUniforms(f.node.get(), sprites, cap1.op());
        REQUIRE(cap1.find(G_M) != nullptr);
        CHECK(cap1.find(G_M)->values[12] == doctest::Approx(1.0f));

        // Move the node — MarkTransDirty bumps the epoch, gate recomputes.
        f.node->SetTranslate(Eigen::Vector3f(9, 0, 0));

        UniformCapture cap2;
        updater.UpdateUniforms(f.node.get(), sprites, cap2.op());
        REQUIRE(cap2.find(G_M) != nullptr);
        CHECK(cap2.find(G_M)->values[12] == doctest::Approx(9.0f));
    }
}

// =============================================================================
//   InitUniforms — confirm the flag/aggregate side-effect that drives the
//   audio FFT gate.  Volumetric test covers audioConsumerPredicate(); this
//   one pins the InitUniforms write into m_hasAudioUniform via the public
//   hasAudioConsumer() readback.
// =============================================================================
TEST_SUITE("WPShaderValueUpdater::Uniforms::init_audio_flag") {
    TEST_CASE("InitUniforms latches m_hasAudioUniform when a spectrum exists") {
        Scene scene;

        auto f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        CHECK_FALSE(updater.hasAudioConsumer()); // baseline: no consumers

        updater.InitUniforms(f.node.get(),
                             makeExistsOp({ G_AUDIOSPECTRUM32LEFT }));
        CHECK(updater.hasAudioConsumer()); // latched by InitUniforms
    }

    TEST_CASE("InitUniforms without any spectrum keeps the gate false") {
        Scene scene;
        auto  f = NodeFixture::make();
        scene.sceneGraph->AppendChild(f.node);

        WPShaderValueUpdater updater(&scene);
        updater.InitUniforms(f.node.get(), makeExistsOp({ G_TIME, G_M }));
        CHECK_FALSE(updater.hasAudioConsumer());
    }
}
