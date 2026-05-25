#include <doctest.h>

#include "wpscene/WPLightObject.hpp"
#include "Fs/VFS.h"

#include <nlohmann/json.hpp>

using namespace wallpaper;
using namespace wallpaper::wpscene;

// WPLightObject::FromJson — tier-1 unit coverage for the JSON pull.
//
// Two newly-added fields (parent_id + exponent) were missing before the
// Real-Time Earth (3557068717) audit: WE parents its 2 sun-lights to an
// animated SUN m5 node 5 levels deep in the scene graph, and authors the
// exponent at 0.1 for a soft long-tail falloff.  Without parent+exponent
// honoring, parented lights resolve at world origin (Earth's center) →
// outward normals dot=0 → planet surface renders pure black; and even with
// the world-position fix, the hardcoded exponent=1.0 produces linear
// falloff instead of the authored soft falloff.
//
// The other fields covered here ride on the pre-existing FromJson tests
// (test_WPSceneParse end-to-end exercise) — this file pins the two
// additions in isolation so a regression on either field surfaces
// immediately.

TEST_SUITE("WPLightObject") {
    TEST_CASE("FromJson parses parent and exponent") {
        fs::VFS    vfs;
        const auto j = nlohmann::json::parse(R"({
            "id": 272,
            "parent": 99,
            "light": "lpoint",
            "color": "1.0 1.0 1.0",
            "origin": "0 0 0",
            "scale": "1 1 1",
            "angles": "0 0 0",
            "radius": 3000.0,
            "intensity": 3.0,
            "exponent": 0.1
        })");
        WPLightObject light;
        REQUIRE(light.FromJson(j, vfs));
        CHECK(light.parent_id == 99);
        CHECK(light.exponent == doctest::Approx(0.1f));
        CHECK(light.radius == doctest::Approx(3000.0f));
        CHECK(light.intensity == doctest::Approx(3.0f));
    }

    TEST_CASE("FromJson defaults parent_id=-1 and exponent=1.0 when absent") {
        fs::VFS    vfs;
        const auto j = nlohmann::json::parse(R"({
            "id": 100,
            "light": "point",
            "color": "1 1 1",
            "origin": "0 0 0",
            "scale": "1 1 1",
            "angles": "0 0 0",
            "radius": 1000.0,
            "intensity": 1.0
        })");
        WPLightObject light;
        REQUIRE(light.FromJson(j, vfs));
        CHECK(light.parent_id == -1);
        CHECK(light.exponent == doctest::Approx(1.0f));
    }

    TEST_CASE("FromJson accepts integer parent value") {
        // JSON-numeric form of parent — most authored scenes use this shape.
        fs::VFS    vfs;
        const auto j = nlohmann::json::parse(R"({
            "id": 1,
            "parent": 42,
            "light": "lpoint",
            "color": "1 1 1",
            "origin": "0 0 0",
            "scale": "1 1 1",
            "angles": "0 0 0",
            "radius": 100.0,
            "intensity": 1.0
        })");
        WPLightObject light;
        REQUIRE(light.FromJson(j, vfs));
        CHECK(light.parent_id == 42);
    }

    TEST_CASE("FromJson parses density and volumetricsexponent") {
        fs::VFS    vfs;
        const auto j = nlohmann::json::parse(R"({
            "id": 50,
            "light": "lpoint",
            "color": "1 1 1",
            "origin": "0 0 0",
            "scale": "1 1 1",
            "angles": "0 0 0",
            "radius": 500.0,
            "intensity": 1.0,
            "density": 7.48,
            "volumetricsexponent": 4.0
        })");
        WPLightObject light;
        REQUIRE(light.FromJson(j, vfs));
        CHECK(light.density == doctest::Approx(7.48f));
        CHECK(light.volumetrics_exponent == doctest::Approx(4.0f));
    }

    TEST_CASE("FromJson defaults density=0 and volumetrics_exponent=1.0 when absent") {
        fs::VFS    vfs;
        const auto j = nlohmann::json::parse(R"({
            "id": 50,
            "light": "lpoint",
            "color": "1 1 1",
            "origin": "0 0 0",
            "scale": "1 1 1",
            "angles": "0 0 0",
            "radius": 500.0,
            "intensity": 1.0
        })");
        WPLightObject light;
        REQUIRE(light.FromJson(j, vfs));
        CHECK(light.density == doctest::Approx(0.0f));
        CHECK(light.volumetrics_exponent == doctest::Approx(1.0f));
    }

    TEST_CASE("FromJson parses castshadow and cascade distances") {
        fs::VFS    vfs;
        const auto j = nlohmann::json::parse(R"({
            "id": 50,
            "light": "lpoint",
            "color": "1 1 1",
            "origin": "0 0 0",
            "scale": "1 1 1",
            "angles": "0 0 0",
            "radius": 500.0,
            "intensity": 1.0,
            "castshadow": true,
            "cascadedistance0": 10.0,
            "cascadedistance1": 50.0,
            "cascadedistance2": 300.0
        })");
        WPLightObject light;
        REQUIRE(light.FromJson(j, vfs));
        CHECK(light.cast_shadow == true);
        CHECK(light.cascade_distances[0] == doctest::Approx(10.0f));
        CHECK(light.cascade_distances[1] == doctest::Approx(50.0f));
        CHECK(light.cascade_distances[2] == doctest::Approx(300.0f));
    }

    TEST_CASE("FromJson defaults cast_shadow=false and cascade_distances=0/100/200") {
        fs::VFS    vfs;
        const auto j = nlohmann::json::parse(R"({
            "id": 50,
            "light": "lpoint",
            "color": "1 1 1",
            "origin": "0 0 0",
            "scale": "1 1 1",
            "angles": "0 0 0",
            "radius": 500.0,
            "intensity": 1.0
        })");
        WPLightObject light;
        REQUIRE(light.FromJson(j, vfs));
        CHECK(light.cast_shadow == false);
        CHECK(light.cascade_distances[0] == doctest::Approx(0.0f));
        CHECK(light.cascade_distances[1] == doctest::Approx(100.0f));
        CHECK(light.cascade_distances[2] == doctest::Approx(200.0f));
    }
}
