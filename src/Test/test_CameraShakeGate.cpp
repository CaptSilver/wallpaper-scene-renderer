#include <doctest.h>

#include "WPShaderValueUpdater.hpp"
#include "Scene/CameraShakeGate.h"
#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneNode.h"
#include "SpecTexs.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace wallpaper;

namespace
{

// A node needs both a mesh and a material or UpdateUniforms() bails out before
// it ever reaches the view-projection block.
std::shared_ptr<SceneNode> makeNode(Scene& scene, const std::string& camera) {
    auto node = std::make_shared<SceneNode>(
        Eigen::Vector3f(100.0f, 50.0f, 0.0f), Eigen::Vector3f(1, 1, 1), Eigen::Vector3f(0, 0, 0));
    auto          mesh = std::make_shared<SceneMesh>();
    SceneMaterial mat;
    mat.name = "shake_mat";
    mesh->AddMaterial(std::move(mat));
    node->AddMesh(mesh);
    if (! camera.empty()) node->SetCamera(camera);
    scene.sceneGraph->AppendChild(node);
    return node;
}

std::shared_ptr<SceneCamera> registerOrthoCamera(Scene& scene, const std::string& name) {
    auto cam = std::make_shared<SceneCamera>(1920, 1080, -1.0f, 1.0f);
    cam->SetDirectLookAt(
        Eigen::Vector3d(0, 0, 1), Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 1, 0));
    scene.cameras[name] = cam;
    return cam;
}

std::shared_ptr<SceneCamera> registerPerspectiveCamera(Scene& scene, const std::string& name) {
    auto cam = std::make_shared<SceneCamera>(16.0f / 9.0f, 0.1f, 5000.0f, 50.0f);
    cam->SetDirectLookAt(
        Eigen::Vector3d(0, 0, 1000), Eigen::Vector3d(0, 0, 0), Eigen::Vector3d(0, 1, 0));
    scene.cameras[name] = cam;
    return cam;
}

// Roughness 0 collapses the sum-of-sinusoids to a single sine pair, so the
// offset is a plain function of the scene clock and never lands on zero for
// the clock values used below.
WPCameraShake activeShake() {
    WPCameraShake s;
    s.enable    = true;
    s.amplitude = 1.0f;
    s.speed     = 1.0f;
    s.roughness = 0.0f;
    return s;
}

ExistsUniformOp vpOnly() {
    return [](std::string_view n) {
        return n == G_VP;
    };
}

std::vector<float> uploadVp(WPShaderValueUpdater& updater, SceneNode* node) {
    std::vector<float> out;
    sprite_map_t       sprites;
    updater.UpdateUniforms(node, sprites, [&out](std::string_view n, const ShaderValue& v) {
        if (n == G_VP) out.assign(v.data(), v.data() + v.size());
    });
    return out;
}

// One frame through a freshly built updater, so the matrix cache never carries
// state between the shaken and unshaken measurement.
std::vector<float> vpForOneFrame(Scene& scene, SceneNode* node, bool shake_enabled) {
    WPShaderValueUpdater updater(&scene);
    auto                 shake = activeShake();
    shake.enable               = shake_enabled;
    updater.SetCameraShake(shake);
    updater.InitUniforms(node, vpOnly());
    updater.FrameBegin();
    return uploadVp(updater, node);
}

} // namespace

TEST_SUITE("CameraShakeReach") {
    TEST_CASE("the scene's active camera shakes") {
        Scene scene;
        scene.elapsingTime = 1.0;
        auto global        = registerOrthoCamera(scene, "global");
        scene.activeCamera = global.get();
        auto node          = makeNode(scene, "");

        CHECK(vpForOneFrame(scene, node.get(), true) != vpForOneFrame(scene, node.get(), false));
    }

    TEST_CASE("the perspective companion of the global camera shakes") {
        // Perspective particle systems render through "global_perspective",
        // which shares the global view; leaving it still strands them while the
        // rest of the scene sways.
        Scene scene;
        scene.elapsingTime = 1.0;
        auto global        = registerOrthoCamera(scene, "global");
        scene.activeCamera = global.get();
        registerPerspectiveCamera(scene, "global_perspective");
        auto node = makeNode(scene, "global_perspective");

        CHECK(vpForOneFrame(scene, node.get(), true) != vpForOneFrame(scene, node.get(), false));
    }

    TEST_CASE("the ortho overlay of a 3D scene shakes") {
        Scene scene;
        scene.elapsingTime = 1.0;
        auto global        = registerPerspectiveCamera(scene, "global");
        scene.activeCamera = global.get();
        registerOrthoCamera(scene, "global_ortho");
        auto node = makeNode(scene, "global_ortho");

        CHECK(vpForOneFrame(scene, node.get(), true) != vpForOneFrame(scene, node.get(), false));
    }

    TEST_CASE("a compose layer's camera shakes because it follows the global camera") {
        // A compose effect gets a scene-sized camera bolted onto the active
        // camera's node and registered as a follower of "global"; it renders the
        // same view, so it must sway with it.
        Scene scene;
        scene.elapsingTime = 1.0;
        auto global        = registerOrthoCamera(scene, "global");
        scene.activeCamera = global.get();
        registerOrthoCamera(scene, "compose_layer_cam");
        scene.linkedCameras["global"] = { "compose_layer_cam" };
        auto node                     = makeNode(scene, "compose_layer_cam");

        CHECK(vpForOneFrame(scene, node.get(), true) != vpForOneFrame(scene, node.get(), false));
    }

    TEST_CASE("a layer-local effect camera stays still") {
        // A non-compose effect chain renders its layer 1:1 into its own
        // intermediate target through a camera sized to that layer.  Shaking it
        // would slide the art inside its own texture and the composite would
        // shift it a second time.
        Scene scene;
        scene.elapsingTime = 1.0;
        auto global        = registerOrthoCamera(scene, "global");
        scene.activeCamera = global.get();
        registerOrthoCamera(scene, "layer_local_cam");
        auto node = makeNode(scene, "layer_local_cam");

        CHECK(vpForOneFrame(scene, node.get(), true) == vpForOneFrame(scene, node.get(), false));
    }

    TEST_CASE("the post-processing camera stays still even if listed as a global follower") {
        Scene scene;
        scene.elapsingTime = 1.0;
        auto global        = registerOrthoCamera(scene, "global");
        scene.activeCamera = global.get();
        registerOrthoCamera(scene, "effect");
        scene.linkedCameras["global"] = { "effect" };
        auto node                     = makeNode(scene, "effect");

        CHECK(vpForOneFrame(scene, node.get(), true) == vpForOneFrame(scene, node.get(), false));
    }

    TEST_CASE("a shaking compose camera is recomputed every frame, not served from the cache") {
        // The dirty gate and the apply site must agree on which cameras shake.
        // If only the apply site moves, the second frame is answered from the
        // matrix cache and the layer freezes at frame one's offset.
        Scene scene;
        auto  global       = registerOrthoCamera(scene, "global");
        scene.activeCamera = global.get();
        registerOrthoCamera(scene, "compose_layer_cam");
        scene.linkedCameras["global"] = { "compose_layer_cam" };
        auto node                     = makeNode(scene, "compose_layer_cam");

        WPShaderValueUpdater updater(&scene);
        updater.SetCameraShake(activeShake());
        updater.InitUniforms(node.get(), vpOnly());

        scene.elapsingTime = 1.0;
        updater.FrameBegin();
        auto first = uploadVp(updater, node.get());

        scene.elapsingTime = 1.7;
        updater.FrameBegin();
        auto second = uploadVp(updater, node.get());

        REQUIRE(! first.empty());
        CHECK(first != second);
    }
}

// The decision on its own, without a scene: the two call sites inside the
// uniform updater both read this, so the table is worth pinning down directly.
TEST_SUITE("CameraFollowsGlobalView") {
    TEST_CASE("the scene-wide camera names follow the global view") {
        CHECK(cameraFollowsGlobalView("", false));
        CHECK(cameraFollowsGlobalView("global", false));
        CHECK(cameraFollowsGlobalView("global_ortho", false));
        CHECK(cameraFollowsGlobalView("global_perspective", false));
    }

    TEST_CASE("a per-node camera follows the global view only while linked to it") {
        CHECK(cameraFollowsGlobalView("0x55f0a1", true));
        CHECK_FALSE(cameraFollowsGlobalView("0x55f0a1", false));
    }

    TEST_CASE("the post-process camera never follows the global view") {
        CHECK_FALSE(cameraFollowsGlobalView("effect", false));
        CHECK_FALSE(cameraFollowsGlobalView("effect", true));
    }

    TEST_CASE("the reflection camera frames its own view") {
        // It renders the mirrored scene into a target that is then sampled at
        // world positions, so it is not a follower of the global view.
        CHECK_FALSE(cameraFollowsGlobalView("reflected_perspective", false));
    }

    TEST_CASE("global view names are recognised apart from the linked-camera list") {
        CHECK(isGlobalViewCameraName(""));
        CHECK(isGlobalViewCameraName("global"));
        CHECK(isGlobalViewCameraName("global_ortho"));
        CHECK(isGlobalViewCameraName("global_perspective"));
        CHECK_FALSE(isGlobalViewCameraName("effect"));
        CHECK_FALSE(isGlobalViewCameraName("globalish"));
        CHECK_FALSE(isGlobalViewCameraName("0x55f0a1"));
        CHECK(isPostProcessCameraName("effect"));
        CHECK_FALSE(isPostProcessCameraName(""));
    }
}
