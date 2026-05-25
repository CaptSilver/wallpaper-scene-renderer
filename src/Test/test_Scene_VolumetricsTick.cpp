#include <doctest.h>

#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneLight.hpp"
#include "Scene/SceneNode.h"

#include <Eigen/Dense>
#include <memory>

using namespace wallpaper;

namespace
{

// Fixture: scene with a single Point light at given origin + radius, camera
// looking at the light origin from a given world position.  All test cases
// set up + tear down via this helper.
//
// SceneCamera has no SetPosition() — use SetDirectLookAt(eye, center, up)
// which writes the eye and triggers Update().
//
// PerLight::light is SceneLight* (raw, non-owning).  Scene owns the
// unique_ptr in scene.lights.
struct TickFixture {
    Scene                        scene;
    std::shared_ptr<SceneCamera> cam;
    std::shared_ptr<SceneNode>   light_node;
    SceneLight*                  light_ptr { nullptr };

    void place(const Eigen::Vector3f& light_origin, float radius,
               const Eigen::Vector3d& cam_position) {
        light_node = std::make_shared<SceneNode>(
            light_origin,
            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
            Eigen::Vector3f { 0.0f, 0.0f, 0.0f });
        auto light =
            std::make_unique<SceneLight>(Eigen::Vector3f(1.0f, 1.0f, 1.0f), radius, 1.0f, 1.0f);
        light->setKind(SceneLight::LightKind::LPoint);
        SceneLight::VolumetricParams vp;
        vp.density = 1.0f;
        light->setVolumetric(vp);
        light->setNode(light_node);
        light_ptr = light.get();
        scene.lights.push_back(std::move(light));

        cam = std::make_shared<SceneCamera>(1920, 1080, 0.1f, 1000.0f);
        cam->SetDirectLookAt(cam_position,
                             Eigen::Vector3d(light_origin.x(),
                                             light_origin.y(),
                                             light_origin.z()),
                             Eigen::Vector3d::UnitY());
        scene.cameras["default"] = cam;
        scene.activeCamera       = cam.get();

        Scene::VolumetricsConfig::PerLight pl;
        pl.light = light_ptr;
        scene.volumetricsConfig.per_light.push_back(std::move(pl));
        scene.volumetricsConfig.enabled = true;
    }
};

} // namespace

TEST_SUITE("Scene_TickVolumetricSelection") {
    TEST_CASE("camera inside sphere flips is_inside_this_frame to true") {
        TickFixture f;
        f.place(Eigen::Vector3f(0, 0, 0), 100.0f, Eigen::Vector3d(10, 0, 0));
        f.scene.TickVolumetricSelection();
        REQUIRE_FALSE(f.scene.volumetricsConfig.per_light.empty());
        CHECK(f.scene.volumetricsConfig.per_light[0].is_inside_this_frame == true);
    }

    TEST_CASE("camera outside sphere keeps is_inside_this_frame false") {
        TickFixture f;
        f.place(Eigen::Vector3f(0, 0, 0), 100.0f, Eigen::Vector3d(200, 0, 0));
        f.scene.TickVolumetricSelection();
        CHECK(f.scene.volumetricsConfig.per_light[0].is_inside_this_frame == false);
    }

    TEST_CASE("camera at exactly radius is treated as outside (strict <)") {
        TickFixture f;
        f.place(Eigen::Vector3f(0, 0, 0), 50.0f, Eigen::Vector3d(50, 0, 0));
        f.scene.TickVolumetricSelection();
        CHECK(f.scene.volumetricsConfig.per_light[0].is_inside_this_frame == false);
    }

    TEST_CASE("respects parent-chain world origin via ModelTrans") {
        // Light at local (5, 0, 0) parented to a node translated to (500, 0, 0).
        // World origin should be (505, 0, 0); camera at (510, 0, 0) with
        // radius 10 -> distance 5 -> INSIDE.
        Scene s;
        auto  parent_node = std::make_shared<SceneNode>(
            Eigen::Vector3f { 500.0f, 0.0f, 0.0f },
            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
            Eigen::Vector3f { 0.0f, 0.0f, 0.0f });
        auto child_node = std::make_shared<SceneNode>(
            Eigen::Vector3f { 5.0f, 0.0f, 0.0f },
            Eigen::Vector3f { 1.0f, 1.0f, 1.0f },
            Eigen::Vector3f { 0.0f, 0.0f, 0.0f });
        parent_node->AppendChild(child_node);
        auto light =
            std::make_unique<SceneLight>(Eigen::Vector3f(1.0f, 1.0f, 1.0f), 10.0f, 1.0f, 1.0f);
        light->setKind(SceneLight::LightKind::LPoint);
        SceneLight::VolumetricParams vp;
        vp.density = 1.0f;
        light->setVolumetric(vp);
        light->setNode(child_node);
        SceneLight* light_ptr = light.get();
        s.lights.push_back(std::move(light));

        auto cam = std::make_shared<SceneCamera>(1920, 1080, 0.1f, 1000.0f);
        cam->SetDirectLookAt(Eigen::Vector3d(510, 0, 0),
                             Eigen::Vector3d(505, 0, 0),
                             Eigen::Vector3d::UnitY());
        s.cameras["default"] = cam;
        s.activeCamera       = cam.get();

        Scene::VolumetricsConfig::PerLight pl;
        pl.light = light_ptr;
        s.volumetricsConfig.per_light.push_back(std::move(pl));

        s.TickVolumetricSelection();
        CHECK(s.volumetricsConfig.per_light[0].is_inside_this_frame == true);
    }

    TEST_CASE("no lights leaves no work and does not crash") {
        Scene s;
        s.TickVolumetricSelection();
        CHECK(s.volumetricsConfig.per_light.empty());
    }

    TEST_CASE("null activeCamera short-circuits without touching per_light state") {
        Scene s;
        Scene::VolumetricsConfig::PerLight pl;
        pl.light                = nullptr;
        pl.is_inside_this_frame = true; // pre-set; tick must NOT flip it
        s.volumetricsConfig.per_light.push_back(std::move(pl));
        s.activeCamera = nullptr;
        s.TickVolumetricSelection();
        CHECK(s.volumetricsConfig.per_light[0].is_inside_this_frame == true);
    }

    TEST_CASE("null per-light pointer is forced to is_inside_this_frame=false") {
        Scene s;
        auto  cam = std::make_shared<SceneCamera>(1920, 1080, 0.1f, 1000.0f);
        cam->SetDirectLookAt(Eigen::Vector3d(0, 0, 5),
                             Eigen::Vector3d(0, 0, 0),
                             Eigen::Vector3d::UnitY());
        s.cameras["default"] = cam;
        s.activeCamera       = cam.get();
        Scene::VolumetricsConfig::PerLight pl;
        pl.light                = nullptr;
        pl.is_inside_this_frame = true; // pre-set; tick must override
        s.volumetricsConfig.per_light.push_back(std::move(pl));
        s.TickVolumetricSelection();
        CHECK(s.volumetricsConfig.per_light[0].is_inside_this_frame == false);
    }

    TEST_CASE("light with no SceneNode is forced to is_inside_this_frame=false") {
        Scene s;
        auto  cam = std::make_shared<SceneCamera>(1920, 1080, 0.1f, 1000.0f);
        cam->SetDirectLookAt(Eigen::Vector3d(0, 0, 5),
                             Eigen::Vector3d(0, 0, 0),
                             Eigen::Vector3d::UnitY());
        s.cameras["default"] = cam;
        s.activeCamera       = cam.get();
        auto light =
            std::make_unique<SceneLight>(Eigen::Vector3f(1.0f, 1.0f, 1.0f), 100.0f, 1.0f, 1.0f);
        // No setNode() — light's node() returns nullptr.
        SceneLight* light_ptr = light.get();
        s.lights.push_back(std::move(light));

        Scene::VolumetricsConfig::PerLight pl;
        pl.light                = light_ptr;
        pl.is_inside_this_frame = true; // pre-set; tick must override
        s.volumetricsConfig.per_light.push_back(std::move(pl));
        s.TickVolumetricSelection();
        CHECK(s.volumetricsConfig.per_light[0].is_inside_this_frame == false);
    }
}
