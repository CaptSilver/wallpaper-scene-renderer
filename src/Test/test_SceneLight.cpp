#include <doctest.h>

#include "Scene/SceneLight.hpp"
#include "Scene/SceneNode.h"

#include <memory>

using namespace wallpaper;

TEST_SUITE("SceneLight") {
    TEST_CASE("constructor caches premultiplied = color * intensity * radius^2") {
        Eigen::Vector3f color(1.0f, 0.5f, 0.25f);
        SceneLight      light(color, 2.0f, 3.0f);
        CHECK(light.color().isApprox(color));
        CHECK(light.radius() == doctest::Approx(2.0f));
        CHECK(light.intensity() == doctest::Approx(3.0f));
        auto p = light.premultipliedColor();
        // r=2, i=3 → r² * i = 12 multiplier
        CHECK(p.x() == doctest::Approx(12.0f));
        CHECK(p.y() == doctest::Approx(6.0f));
        CHECK(p.z() == doctest::Approx(3.0f));
    }

    TEST_CASE("colorIntensity multiplies color by intensity") {
        SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 0.5f);
        auto       ci = light.colorIntensity();
        CHECK(ci.x() == doctest::Approx(0.5f));
        CHECK(ci.y() == doctest::Approx(0.5f));
        CHECK(ci.z() == doctest::Approx(0.5f));
    }

    TEST_CASE("setColor re-computes premultiplied") {
        SceneLight light(Eigen::Vector3f(1, 0, 0), 2.0f, 1.0f);
        // initial: premul = (1,0,0) * 1 * 4 = (4, 0, 0)
        CHECK(light.premultipliedColor().x() == doctest::Approx(4.0f));
        light.setColor(Eigen::Vector3f(0, 1, 0));
        CHECK(light.premultipliedColor().y() == doctest::Approx(4.0f));
        CHECK(light.premultipliedColor().x() == doctest::Approx(0.0f));
    }

    TEST_CASE("setRadius recomputes premultiplied (radius squared)") {
        SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        CHECK(light.premultipliedColor().x() == doctest::Approx(1.0f));
        light.setRadius(3.0f);
        CHECK(light.premultipliedColor().x() == doctest::Approx(9.0f)); // 3² = 9
    }

    TEST_CASE("setIntensity scales premultiplied linearly") {
        SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        light.setIntensity(5.0f);
        CHECK(light.premultipliedColor().x() == doctest::Approx(5.0f));
    }

    TEST_CASE("setNode stores shared pointer and node() retrieves raw") {
        SceneLight light(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        CHECK(light.node() == nullptr);
        auto node = std::make_shared<SceneNode>();
        light.setNode(node);
        CHECK(light.node() == node.get());
    }

    // Per-light falloff exponent — Real-Time Earth (3557068717) authors 0.1
    // for a soft long-tail falloff.  ComputePBRLightShadow reads it as
    // `radiance = color * pow(saturate(1 - d/radius), exponent)`.  Default
    // 1.0 (linear) keeps legacy scenes unchanged.
    TEST_CASE("constructor stores exponent and exponent() returns it") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 100.0f, 2.0f, 0.1f);
        CHECK(l.exponent() == doctest::Approx(0.1f));
    }

    TEST_CASE("exponent defaults to 1.0 (linear falloff) when not provided") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 100.0f, 2.0f);
        CHECK(l.exponent() == doctest::Approx(1.0f));
    }

    TEST_CASE("LightKind defaults to Point") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        CHECK(l.kind() == SceneLight::LightKind::Point);
    }

    TEST_CASE("setKind round-trips through getter") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        l.setKind(SceneLight::LightKind::LPoint);
        CHECK(l.kind() == SceneLight::LightKind::LPoint);
        l.setKind(SceneLight::LightKind::LSpot);
        CHECK(l.kind() == SceneLight::LightKind::LSpot);
        l.setKind(SceneLight::LightKind::LTube);
        CHECK(l.kind() == SceneLight::LightKind::LTube);
        l.setKind(SceneLight::LightKind::LDirectional);
        CHECK(l.kind() == SceneLight::LightKind::LDirectional);
    }

    TEST_CASE("VolumetricParams default-constructs to absent/zero/linear") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        const auto& v = l.volumetric();
        CHECK(v.cast_volumetrics_explicit == false);
        CHECK(v.cast_volumetrics_value == false);
        CHECK(v.density == doctest::Approx(0.0f));
        CHECK(v.exponent == doctest::Approx(1.0f));
    }

    TEST_CASE("setVolumetric round-trips POD through getter") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.cast_volumetrics_explicit = true;
        vp.cast_volumetrics_value    = true;
        vp.density                   = 7.48f;
        vp.exponent                  = 4.0f;
        l.setVolumetric(vp);
        const auto& got = l.volumetric();
        CHECK(got.cast_volumetrics_explicit == true);
        CHECK(got.cast_volumetrics_value == true);
        CHECK(got.density == doctest::Approx(7.48f));
        CHECK(got.exponent == doctest::Approx(4.0f));
    }

} // SceneLight
