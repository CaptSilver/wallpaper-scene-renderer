#include <doctest.h>

#include "wpscene/WPScene.h"

#include <nlohmann/json.hpp>

using namespace wallpaper::wpscene;

namespace
{

const char* kMinimalScene = R"({
    "camera":  {"center": "0 0 0", "eye": "0 0 1", "up": "0 1 0"},
    "general": {
        "ambientcolor":  "0.1 0.2 0.3",
        "skylightcolor": "0.4 0.5 0.6",
        "clearcolor":    "0.7 0.8 0.9",
        "cameraparallax":               false,
        "cameraparallaxamount":         0.0,
        "cameraparallaxdelay":          0.0,
        "cameraparallaxmouseinfluence": 0.0
    }
})";

}

TEST_SUITE("Orthogonalprojection::FromJson") {
    TEST_CASE("null json is rejected") {
        Orthogonalprojection o;
        CHECK_FALSE(o.FromJson(nlohmann::json()));
    }

    TEST_CASE("auto:true sets auto_") {
        auto j = nlohmann::json::parse(R"({"auto": true})");
        Orthogonalprojection o;
        REQUIRE(o.FromJson(j));
        CHECK(o.auto_ == true);
    }

    TEST_CASE("explicit width/height when auto absent") {
        auto j = nlohmann::json::parse(R"({"width": 1280, "height": 720})");
        Orthogonalprojection o;
        REQUIRE(o.FromJson(j));
        CHECK(o.width == 1280);
        CHECK(o.height == 720);
    }

    TEST_CASE("postprocessing field captured when present") {
        auto j = nlohmann::json::parse(R"({"width": 100, "height": 100, "postprocessing": "ultra"})");
        Orthogonalprojection o;
        REQUIRE(o.FromJson(j));
        CHECK(o.postprocessing == "ultra");
    }
}

TEST_SUITE("WPSceneCamera::FromJson") {
    TEST_CASE("paths array populated") {
        auto j = nlohmann::json::parse(R"({
            "center": "1 2 3", "eye": "4 5 6", "up": "0 1 0",
            "paths": ["a", "b", "c"]
        })");
        WPSceneCamera c;
        REQUIRE(c.FromJson(j));
        REQUIRE(c.paths.size() == 3u);
        CHECK(c.paths[0] == "a");
        CHECK(c.paths[2] == "c");
    }

    TEST_CASE("paths absent → empty vector") {
        auto j = nlohmann::json::parse(R"({
            "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0"
        })");
        WPSceneCamera c;
        REQUIRE(c.FromJson(j));
        CHECK(c.paths.empty());
    }

    TEST_CASE("non-string entries in paths are silently dropped") {
        auto j = nlohmann::json::parse(R"({
            "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0",
            "paths": ["str1", 42, "str2"]
        })");
        WPSceneCamera c;
        REQUIRE(c.FromJson(j));
        REQUIRE(c.paths.size() == 2u);
        CHECK(c.paths[0] == "str1");
        CHECK(c.paths[1] == "str2");
    }
}

TEST_SUITE("WPSceneGeneral::FromJson") {
    TEST_CASE("required color fields parse") {
        auto j = nlohmann::json::parse(kMinimalScene);
        WPSceneGeneral g;
        REQUIRE(g.FromJson(j.at("general")));
        CHECK(g.ambientcolor[0] == doctest::Approx(0.1f));
        CHECK(g.ambientcolor[1] == doctest::Approx(0.2f));
        CHECK(g.ambientcolor[2] == doctest::Approx(0.3f));
        CHECK(g.clearcolor[0] == doctest::Approx(0.7f));
    }

    TEST_CASE("optional bloom/hdr fields default off when absent") {
        auto j = nlohmann::json::parse(kMinimalScene);
        WPSceneGeneral g;
        REQUIRE(g.FromJson(j.at("general")));
        CHECK_FALSE(g.hdr);
        CHECK_FALSE(g.bloom);
        CHECK(g.bloomstrength == doctest::Approx(2.0f));
    }

    TEST_CASE("hdr/bloom fields parse when present") {
        auto j = nlohmann::json::parse(R"({
            "ambientcolor":"0 0 0","skylightcolor":"0 0 0","clearcolor":"0 0 0",
            "cameraparallax": false, "cameraparallaxamount": 0,
            "cameraparallaxdelay": 0, "cameraparallaxmouseinfluence": 0,
            "hdr": true, "bloom": true,
            "bloomstrength": 3.5, "bloomthreshold": 0.4,
            "bloomhdrstrength": 4.0, "bloomhdrthreshold": 1.5
        })");
        WPSceneGeneral g;
        REQUIRE(g.FromJson(j));
        CHECK(g.hdr);
        CHECK(g.bloom);
        CHECK(g.bloomstrength == doctest::Approx(3.5f));
        CHECK(g.bloomthreshold == doctest::Approx(0.4f));
        CHECK(g.bloomhdrstrength == doctest::Approx(4.0f));
        CHECK(g.bloomhdrthreshold == doctest::Approx(1.5f));
    }

    TEST_CASE("camerashake fields parse") {
        auto j = nlohmann::json::parse(R"({
            "ambientcolor":"0 0 0","skylightcolor":"0 0 0","clearcolor":"0 0 0",
            "cameraparallax": false, "cameraparallaxamount": 0,
            "cameraparallaxdelay": 0, "cameraparallaxmouseinfluence": 0,
            "camerashake": true,
            "camerashakeamplitude": 1.0, "camerashakespeed": 2.0,
            "camerashakeroughness": 0.25
        })");
        WPSceneGeneral g;
        REQUIRE(g.FromJson(j));
        CHECK(g.camerashake);
        CHECK(g.camerashakeamplitude == doctest::Approx(1.0f));
        CHECK(g.camerashakespeed == doctest::Approx(2.0f));
        CHECK(g.camerashakeroughness == doctest::Approx(0.25f));
    }

    TEST_CASE("camerafade flag parses") {
        auto j = nlohmann::json::parse(R"({
            "ambientcolor":"0 0 0","skylightcolor":"0 0 0","clearcolor":"0 0 0",
            "cameraparallax": false, "cameraparallaxamount": 0,
            "cameraparallaxdelay": 0, "cameraparallaxmouseinfluence": 0,
            "camerafade": true
        })");
        WPSceneGeneral g;
        REQUIRE(g.FromJson(j));
        CHECK(g.camerafade);
    }

    TEST_CASE("orthogonalprojection nested object marks isOrtho") {
        auto j = nlohmann::json::parse(R"({
            "ambientcolor":"0 0 0","skylightcolor":"0 0 0","clearcolor":"0 0 0",
            "cameraparallax": false, "cameraparallaxamount": 0,
            "cameraparallaxdelay": 0, "cameraparallaxmouseinfluence": 0,
            "orthogonalprojection": {"width": 800, "height": 600}
        })");
        WPSceneGeneral g;
        REQUIRE(g.FromJson(j));
        CHECK(g.isOrtho);
        CHECK(g.orthogonalprojection.width == 800);
    }

    TEST_CASE("orthogonalprojection: null disables isOrtho") {
        auto j = nlohmann::json::parse(R"({
            "ambientcolor":"0 0 0","skylightcolor":"0 0 0","clearcolor":"0 0 0",
            "cameraparallax": false, "cameraparallaxamount": 0,
            "cameraparallaxdelay": 0, "cameraparallaxmouseinfluence": 0,
            "orthogonalprojection": null
        })");
        WPSceneGeneral g;
        REQUIRE(g.FromJson(j));
        CHECK_FALSE(g.isOrtho);
    }

    TEST_CASE("orthogonalprojection absent — isOrtho false branch") {
        auto j = nlohmann::json::parse(kMinimalScene);
        WPSceneGeneral g;
        REQUIRE(g.FromJson(j.at("general")));
        CHECK_FALSE(g.isOrtho);
    }

    TEST_CASE("gravity/wind globals parse") {
        auto j = nlohmann::json::parse(R"({
            "ambientcolor":"0 0 0","skylightcolor":"0 0 0","clearcolor":"0 0 0",
            "cameraparallax": false, "cameraparallaxamount": 0,
            "cameraparallaxdelay": 0, "cameraparallaxmouseinfluence": 0,
            "gravitydirection": "0 -2 0", "gravitystrength": 9.8,
            "winddirection": "1 0 0", "windenabled": true, "windstrength": 5.0
        })");
        WPSceneGeneral g;
        REQUIRE(g.FromJson(j));
        CHECK(g.gravitydirection[1] == doctest::Approx(-2.0f));
        CHECK(g.gravitystrength == doctest::Approx(9.8f));
        CHECK(g.winddirection[0] == doctest::Approx(1.0f));
        CHECK(g.windenabled);
        CHECK(g.windstrength == doctest::Approx(5.0f));
    }
}

TEST_SUITE("WPScene::FromJson") {
    TEST_CASE("missing camera is rejected") {
        auto j = nlohmann::json::parse(R"({"general": {}})");
        WPScene s;
        CHECK_FALSE(s.FromJson(j));
    }

    TEST_CASE("missing general is rejected") {
        auto j = nlohmann::json::parse(R"({"camera": {"center":"0 0 0","eye":"0 0 1","up":"0 1 0"}})");
        WPScene s;
        CHECK_FALSE(s.FromJson(j));
    }

    TEST_CASE("complete scene parses both subobjects") {
        auto j = nlohmann::json::parse(kMinimalScene);
        WPScene s;
        REQUIRE(s.FromJson(j));
        CHECK(s.camera.eye[2] == doctest::Approx(1.0f));
        CHECK(s.general.ambientcolor[1] == doctest::Approx(0.2f));
    }
}
