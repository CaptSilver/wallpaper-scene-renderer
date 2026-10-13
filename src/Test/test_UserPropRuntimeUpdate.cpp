#include <doctest.h>

#include "UserPropRuntimeUpdate.hpp"

#include "Scene/Scene.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"

#include <algorithm>

using namespace wallpaper;

namespace
{

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

WPUserProperties loaded(const char* projectJson) {
    WPUserProperties props;
    REQUIRE(props.LoadFromProjectJson(projectJson));
    return props;
}

} // namespace

TEST_SUITE("UserPropRuntimeUpdate") {
    TEST_CASE("only the properties whose value moved count as changed") {
        auto current = loaded(R"({"general":{"properties":{
            "earthcolor": {"type":"color","value":"1 1 1"},
            "cloudopacity": {"type":"slider","value":0.95}
        }}})");

        auto changed = wek::changedUserProps(
            current,
            nlohmann::json::parse(R"({"earthcolor":"1 1 1","cloudopacity":0.5,"newprop":true})"));

        CHECK_FALSE(contains(changed, "earthcolor"));
        CHECK(contains(changed, "cloudopacity"));
        CHECK(contains(changed, "newprop"));
    }

    TEST_CASE("a changed property nothing in the live scene reads forces a reload") {
        Scene scene;
        auto  mesh = std::make_shared<SceneMesh>();
        mesh->AddMaterial(SceneMaterial {});
        scene.userPropUniformBindings["earthcolor"].push_back({ mesh, "g_Color" });
        scene.userPropVisBindings["showclouds"].push_back({ nullptr, true, "", "{}" });
        TextLayerInfo text;
        text.pointsizeUserProp = "clocksize";
        scene.textLayers.push_back(text);

        // Only a property script reads this one, and its scriptProperties are
        // seeded when the script compiles — nothing re-seeds them in place.
        auto needReload = wek::userPropsNeedingReload(
            scene, { "earthcolor", "showclouds", "clocksize", "hudspeed" });

        CHECK(needReload == std::vector<std::string> { "hudspeed" });
    }

    TEST_CASE("an effect-level user-bound visibility switch has a runtime binding too") {
        Scene scene;
        scene.userPropEffectVisBindings["godraysswordtree_placeholder"].push_back(
            { 163, 0, true, "{}" });

        auto needReload = wek::userPropsNeedingReload(scene, { "godraysswordtree_placeholder" });

        CHECK(needReload.empty());
    }

    TEST_CASE("a binding at a layer's terminal effect index needs no reload either") {
        // Regression guard for retiring the terminal-position full-reload
        // fallback: userPropHasRuntimeBinding never special-cased effectIndex
        // by position, so once ScriptEffectVisResolve/UserPropEffectVisResolve
        // stopped dropping the terminal binding at the parser layer (the case
        // this guards), the map simply gains an entry the parser used to
        // withhold -- this function's own behaviour needs no change. Index 2
        // mirrors Album Cover's "Previous" position (a 3-effect layer's
        // terminal).
        Scene scene;
        scene.userPropEffectVisBindings["mediaplayer_previous_enable"].push_back(
            { 174, 2, true, "{}" });

        auto needReload = wek::userPropsNeedingReload(scene, { "mediaplayer_previous_enable" });

        CHECK(needReload.empty());
    }

    TEST_CASE("a property that picks a texture variant reloads even with a live binding") {
        // The variant is chosen when the .tex is decoded; nothing re-decodes
        // it in place, so a visibility binding on the same combo is not enough.
        Scene scene;
        scene.userPropVisBindings["facestyles"].push_back({ nullptr, true, "Isa", "{}" });
        scene.userPropStaticUses["facestyles"].insert("texture variant");

        auto needReload = wek::userPropsNeedingReload(scene, { "facestyles" });

        CHECK(needReload == std::vector<std::string> { "facestyles" });
    }

    // One property can feed a live-updatable field and a parse-time one. Pushing
    // the new value to the live field alone leaves the other stale, so any
    // static use sends the whole property through a reload.
    TEST_CASE("a property with a uniform binding and a static use reloads") {
        Scene scene;
        auto  mesh = std::make_shared<SceneMesh>();
        mesh->AddMaterial(SceneMaterial {});
        scene.userPropUniformBindings["audiocolor"].push_back({ mesh, "g_Color" });
        scene.userPropStaticUses["audiocolor"].insert("color");

        CHECK(wek::userPropsNeedingReload(scene, { "audiocolor" }) ==
              std::vector<std::string> { "audiocolor" });
    }

    TEST_CASE("a property with a visibility binding and a static use reloads") {
        Scene scene;
        scene.userPropVisBindings["depth"].push_back({ nullptr, true, "", "{}" });
        scene.userPropStaticUses["depth"].insert("parallaxDepth");

        CHECK(wek::userPropsNeedingReload(scene, { "depth" }) ==
              std::vector<std::string> { "depth" });
    }

    TEST_CASE("a property whose only use is static reloads") {
        Scene scene;
        scene.userPropStaticUses["objectcolor"].insert("color");

        CHECK(wek::userPropsNeedingReload(scene, { "objectcolor" }) ==
              std::vector<std::string> { "objectcolor" });
    }

    TEST_CASE("a property reported with several static fields reloads once") {
        Scene scene;
        scene.userPropStaticUses["multi"] = { "color", "alpha", "scriptproperties" };

        CHECK(wek::userPropsNeedingReload(scene, { "multi" }).size() == 1u);
    }

    TEST_CASE("a property with only runtime bindings stays live next to a static one") {
        Scene scene;
        scene.userPropVisBindings["live"].push_back({ nullptr, true, "", "{}" });
        scene.userPropStaticUses["baked"].insert("color");

        CHECK(wek::userPropsNeedingReload(scene, { "live", "baked" }) ==
              std::vector<std::string> { "baked" });
    }

    TEST_CASE("the static fields of a property are reported for the reload log") {
        Scene scene;
        scene.userPropStaticUses["p"] = { "color", "parallaxDepth" };

        CHECK(wek::userPropStaticFields(scene, "p") == "color, parallaxDepth");
        CHECK(wek::userPropStaticFields(scene, "unknown").empty());
    }
}

TEST_SUITE("ResolveVisibilityFromJson") {
    TEST_CASE("a JSON bool passes through") {
        CHECK(wek::ResolveVisibilityFromJson(nlohmann::json(true), false));
        CHECK_FALSE(wek::ResolveVisibilityFromJson(nlohmann::json(false), true));
    }
    TEST_CASE("a JSON number is visible unless exactly zero") {
        CHECK(wek::ResolveVisibilityFromJson(nlohmann::json(1.0), false));
        CHECK_FALSE(wek::ResolveVisibilityFromJson(nlohmann::json(0.0), true));
    }
    TEST_CASE("string special cases: empty, \"0\", and \"false\" are all not-visible") {
        CHECK_FALSE(wek::ResolveVisibilityFromJson(nlohmann::json(""), true));
        CHECK_FALSE(wek::ResolveVisibilityFromJson(nlohmann::json("0"), true));
        CHECK_FALSE(wek::ResolveVisibilityFromJson(nlohmann::json("false"), true));
        CHECK(wek::ResolveVisibilityFromJson(nlohmann::json("yes"), false));
    }
    TEST_CASE("an unsupported JSON type falls back to the caller's default") {
        CHECK(wek::ResolveVisibilityFromJson(nlohmann::json::object(), true));
        CHECK_FALSE(wek::ResolveVisibilityFromJson(nlohmann::json::object(), false));
    }
}

TEST_SUITE("ParseFloatVectorFromJson") {
    TEST_CASE("a bare number becomes a one-element vector") {
        auto v = wek::ParseFloatVectorFromJson(nlohmann::json(3.5));
        CHECK(v == std::vector<float> { 3.5f });
    }
    TEST_CASE("a whitespace-separated string parses each float") {
        auto v = wek::ParseFloatVectorFromJson(nlohmann::json("1.0 2.0 3.0"));
        CHECK(v == std::vector<float> { 1.0f, 2.0f, 3.0f });
    }
    TEST_CASE("an array skips its non-numeric elements") {
        auto v = wek::ParseFloatVectorFromJson(nlohmann::json::parse(R"([1, "x", 2])"));
        CHECK(v == std::vector<float> { 1.0f, 2.0f });
    }
    TEST_CASE("an unsupported type produces an empty vector") {
        auto v = wek::ParseFloatVectorFromJson(nlohmann::json::object());
        CHECK(v.empty());
    }
}
