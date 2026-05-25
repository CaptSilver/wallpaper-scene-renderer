#include <doctest.h>

#include "WPLightKindParse.hpp"
#include "Scene/SceneLight.hpp"

using namespace wallpaper;

TEST_SUITE("parseLightKind") {
    TEST_CASE("recognises lpoint / lspot / ltube / ldirectional") {
        CHECK(parseLightKind("lpoint")       == SceneLight::LightKind::LPoint);
        CHECK(parseLightKind("lspot")        == SceneLight::LightKind::LSpot);
        CHECK(parseLightKind("ltube")        == SceneLight::LightKind::LTube);
        CHECK(parseLightKind("ldirectional") == SceneLight::LightKind::LDirectional);
    }

    TEST_CASE("legacy point string maps to Point") {
        CHECK(parseLightKind("point") == SceneLight::LightKind::Point);
    }

    TEST_CASE("empty string defaults to Point") {
        CHECK(parseLightKind("") == SceneLight::LightKind::Point);
    }

    TEST_CASE("unknown string defaults to Point") {
        // Workshop scenes occasionally author a future-kind value our parser
        // doesn't recognize; we silently fall back to Point so the scene at
        // least loads.
        CHECK(parseLightKind("unobtainable") == SceneLight::LightKind::Point);
        CHECK(parseLightKind("LPOINT")       == SceneLight::LightKind::Point); // case-sensitive
    }
}
