#include <doctest.h>

#include "Scene/SceneLight.hpp"
#include "Scene/SceneNode.h"

#include <cstdlib>
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

    TEST_CASE("castShadow defaults false and round-trips") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        CHECK(l.castShadow() == false);
        l.setCastShadow(true);
        CHECK(l.castShadow() == true);
    }

    TEST_CASE("cascadeDistances defaults to 0/100/200 and round-trips") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        const auto& def = l.cascadeDistances();
        CHECK(def[0] == doctest::Approx(0.0f));
        CHECK(def[1] == doctest::Approx(100.0f));
        CHECK(def[2] == doctest::Approx(200.0f));
        l.setCascadeDistances(std::array<float, 3> { 10.0f, 50.0f, 300.0f });
        const auto& got = l.cascadeDistances();
        CHECK(got[0] == doctest::Approx(10.0f));
        CHECK(got[1] == doctest::Approx(50.0f));
        CHECK(got[2] == doctest::Approx(300.0f));
    }

    TEST_CASE("castsVolumetrics: default-constructed light does not cast") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        CHECK(l.castsVolumetrics() == false);
    }

    TEST_CASE("castsVolumetrics: density>0 with no explicit flag casts via heuristic") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.density = 5.0f;
        l.setVolumetric(vp);
        CHECK(l.castsVolumetrics() == true);
    }

    TEST_CASE("castsVolumetrics: density=0 with no explicit flag does not cast") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.density = 0.0f;
        l.setVolumetric(vp);
        CHECK(l.castsVolumetrics() == false);
    }

    TEST_CASE("castsVolumetrics: explicit:false overrides density>0") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.cast_volumetrics_explicit = true;
        vp.cast_volumetrics_value    = false;
        vp.density                   = 5.0f;
        l.setVolumetric(vp);
        CHECK(l.castsVolumetrics() == false);
    }

    TEST_CASE("castsVolumetrics: explicit:true with density=0 still does not cast") {
        // The predicate cheaply gates on density>0 even on explicit-true since a
        // zero-density pass produces a zero buffer (wasted GPU work).
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.cast_volumetrics_explicit = true;
        vp.cast_volumetrics_value    = true;
        vp.density                   = 0.0f;
        l.setVolumetric(vp);
        CHECK(l.castsVolumetrics() == false);
    }

    TEST_CASE("castsVolumetrics: editor default density=2.0 casts via heuristic") {
        // 5 of the 9 workshop scenes carrying volumetric fields use the
        // editor's emit-on-toggle default density=2.0 without authoring
        // castvolumetrics; the heuristic intentionally opts them in.
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.density = 2.0f;
        l.setVolumetric(vp);
        CHECK(l.castsVolumetrics() == true);
    }

    TEST_CASE("isVolumetricEmitterCandidate: castsVolumetrics + Point is true") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.density = 2.0f;
        l.setVolumetric(vp);
        // Default kind is Point.
        REQUIRE(l.castsVolumetrics() == true);
        CHECK(l.isVolumetricEmitterCandidate() == true);
    }

    TEST_CASE("isVolumetricEmitterCandidate: castsVolumetrics + LPoint is true") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.density = 2.0f;
        l.setVolumetric(vp);
        l.setKind(SceneLight::LightKind::LPoint);
        CHECK(l.isVolumetricEmitterCandidate() == true);
    }

    TEST_CASE("isVolumetricEmitterCandidate: castsVolumetrics but LSpot/LTube/LDir is false") {
        // Predicate gate matches the per-frame uniform writer: today the chain
        // only emits Point/LPoint, so non-Point lights with volumetric fields
        // must be filtered out before pipeline construction.
        for (auto k : { SceneLight::LightKind::LSpot,
                        SceneLight::LightKind::LTube,
                        SceneLight::LightKind::LDirectional }) {
            SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
            SceneLight::VolumetricParams vp;
            vp.density = 5.0f;
            l.setVolumetric(vp);
            l.setKind(k);
            REQUIRE(l.castsVolumetrics() == true);
            CHECK(l.isVolumetricEmitterCandidate() == false);
        }
    }

    TEST_CASE("isVolumetricEmitterCandidate: no-cast Point is false") {
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        // density=0 (default) — does not cast even though kind is Point.
        REQUIRE(l.castsVolumetrics() == false);
        CHECK(l.isVolumetricEmitterCandidate() == false);
    }

    TEST_CASE("env WEKDE_VOLUMETRICS=force-off suppresses density>0 + explicit:true") {
        setenv("WEKDE_VOLUMETRICS", "force-off", 1);
        SceneLight::_resetVolumetricsOverrideForTesting();
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.cast_volumetrics_explicit = true;
        vp.cast_volumetrics_value    = true;
        vp.density                   = 5.0f;
        l.setVolumetric(vp);
        CHECK(l.castsVolumetrics() == false);
        unsetenv("WEKDE_VOLUMETRICS");
        SceneLight::_resetVolumetricsOverrideForTesting();
    }

    TEST_CASE("env WEKDE_VOLUMETRICS=force-on requires density>0 but ignores explicit:false") {
        setenv("WEKDE_VOLUMETRICS", "force-on", 1);
        SceneLight::_resetVolumetricsOverrideForTesting();
        // density=0 — still off.
        {
            SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
            SceneLight::VolumetricParams vp;
            vp.density = 0.0f;
            l.setVolumetric(vp);
            CHECK(l.castsVolumetrics() == false);
        }
        // density>0 with explicit:false — force-on bypasses the suppression.
        {
            SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
            SceneLight::VolumetricParams vp;
            vp.cast_volumetrics_explicit = true;
            vp.cast_volumetrics_value    = false;
            vp.density                   = 5.0f;
            l.setVolumetric(vp);
            CHECK(l.castsVolumetrics() == true);
        }
        unsetenv("WEKDE_VOLUMETRICS");
        SceneLight::_resetVolumetricsOverrideForTesting();
    }

    TEST_CASE("env WEKDE_VOLUMETRICS=auto matches the no-env default") {
        setenv("WEKDE_VOLUMETRICS", "auto", 1);
        SceneLight::_resetVolumetricsOverrideForTesting();
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.density = 5.0f;
        l.setVolumetric(vp);
        CHECK(l.castsVolumetrics() == true);  // heuristic on density>0
        unsetenv("WEKDE_VOLUMETRICS");
        SceneLight::_resetVolumetricsOverrideForTesting();
    }

    TEST_CASE("env WEKDE_VOLUMETRICS=invalid falls back to Auto") {
        setenv("WEKDE_VOLUMETRICS", "garbage", 1);
        SceneLight::_resetVolumetricsOverrideForTesting();
        SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
        SceneLight::VolumetricParams vp;
        vp.density = 5.0f;
        l.setVolumetric(vp);
        CHECK(l.castsVolumetrics() == true);  // heuristic still applies
        unsetenv("WEKDE_VOLUMETRICS");
        SceneLight::_resetVolumetricsOverrideForTesting();
    }

    TEST_CASE("env WEKDE_VOLUMETRICS aliases: 0 -> force-off, 1 -> force-on") {
        setenv("WEKDE_VOLUMETRICS", "0", 1);
        SceneLight::_resetVolumetricsOverrideForTesting();
        {
            SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
            SceneLight::VolumetricParams vp;
            vp.density = 5.0f;
            l.setVolumetric(vp);
            CHECK(l.castsVolumetrics() == false);  // 0 == force-off
        }
        setenv("WEKDE_VOLUMETRICS", "1", 1);
        SceneLight::_resetVolumetricsOverrideForTesting();
        {
            SceneLight l(Eigen::Vector3f(1, 1, 1), 1.0f, 1.0f);
            SceneLight::VolumetricParams vp;
            vp.cast_volumetrics_explicit = true;
            vp.cast_volumetrics_value    = false;
            vp.density                   = 5.0f;
            l.setVolumetric(vp);
            CHECK(l.castsVolumetrics() == true);   // 1 == force-on
        }
        unsetenv("WEKDE_VOLUMETRICS");
        SceneLight::_resetVolumetricsOverrideForTesting();
    }

} // SceneLight
