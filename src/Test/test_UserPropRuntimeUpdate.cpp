#include <doctest.h>

#include "UserPropRuntimeUpdate.hpp"

#include "Scene/Scene.h"
#include "Scene/SceneMaterial.h"

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
        Scene         scene;
        SceneMaterial mat;
        scene.userPropUniformBindings["earthcolor"].push_back({ &mat, "g_Color" });
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
}
