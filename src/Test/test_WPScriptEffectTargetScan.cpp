#include <doctest.h>

#include "WPScriptEffectTargetScan.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace wek;
using nlohmann::json;
using wallpaper::i32;

TEST_SUITE("WPScriptEffectTargetScan") {
    TEST_CASE("an index loop over thisLayer's effects targets the script's own layer") {
        auto t =
            scanScriptEffectVisibility("for (var i = 0; i < thisLayer.getEffectCount(); i++) {"
                                       "  var e = thisLayer.getEffect(i); e.visible = false; }");
        REQUIRE(t.has_value());
        CHECK(t->ownLayer);
        CHECK(t->layerNames.empty());
        CHECK_FALSE(t->unresolved);
    }

    TEST_CASE("thisObject targets the script's own layer too") {
        auto t = scanScriptEffectVisibility("thisObject.getEffect('a').visible = true;");
        REQUIRE(t.has_value());
        CHECK(t->ownLayer);
    }

    TEST_CASE("a string-literal getLayer names the target layer") {
        auto t = scanScriptEffectVisibility(
            "thisScene.getLayer('Room interior').getEffect('Radio Display').visible = on;");
        REQUIRE(t.has_value());
        CHECK_FALSE(t->ownLayer);
        REQUIRE(t->layerNames.size() == 1u);
        CHECK(t->layerNames[0] == "Room interior");
        CHECK_FALSE(t->unresolved);
    }

    TEST_CASE("a double-quoted layer name is read the same way") {
        auto t = scanScriptEffectVisibility(
            "thisScene.getLayer(\"vinyl\").getEffect(0).visible = false;");
        REQUIRE(t.has_value());
        REQUIRE(t->layerNames.size() == 1u);
        CHECK(t->layerNames[0] == "vinyl");
    }

    TEST_CASE("a layer alias assigned from a literal getLayer resolves to that name") {
        auto t = scanScriptEffectVisibility("coverLayer = thisScene.getLayer('default_cover');"
                                            "coverLayer.getEffect('blur').visible = false;");
        REQUIRE(t.has_value());
        CHECK_FALSE(t->ownLayer);
        REQUIRE(t->layerNames.size() == 1u);
        CHECK(t->layerNames[0] == "default_cover");
        CHECK_FALSE(t->unresolved);
    }

    TEST_CASE("an alias of thisLayer targets the script's own layer") {
        auto t =
            scanScriptEffectVisibility("var self = thisLayer; self.getEffect(0).visible = false;");
        REQUIRE(t.has_value());
        CHECK(t->ownLayer);
        CHECK(t->layerNames.empty());
        CHECK_FALSE(t->unresolved);
    }

    TEST_CASE("a script that only names other layers by literal does not target its host") {
        auto t = scanScriptEffectVisibility("thisScene.getLayer('A').getEffect(0).visible = false;"
                                            "thisScene.getLayer('B').getEffect(1).visible = true;");
        REQUIRE(t.has_value());
        CHECK_FALSE(t->ownLayer);
        CHECK(t->layerNames.size() == 2u);
    }

    TEST_CASE("each literal layer name is listed once") {
        auto t = scanScriptEffectVisibility("thisScene.getLayer('A').getEffect(0).visible = false;"
                                            "thisScene.getLayer('A').getEffect(1).visible = true;");
        REQUIRE(t.has_value());
        CHECK(t->layerNames.size() == 1u);
    }

    TEST_CASE("getEffect without a visible write registers nothing") {
        CHECK_FALSE(
            scanScriptEffectVisibility("thisLayer.getEffect('a').getMaterial().setValue('x', 1);")
                .has_value());
    }

    TEST_CASE("a visible write without getEffect registers nothing") {
        CHECK_FALSE(scanScriptEffectVisibility("thisLayer.visible = false;").has_value());
    }

    TEST_CASE("visible comparisons are not writes") {
        CHECK_FALSE(scanScriptEffectVisibility("if (thisLayer.getEffect(0).visible == true) {}")
                        .has_value());
        CHECK_FALSE(scanScriptEffectVisibility("if (thisLayer.getEffect(0).visible === true) {}")
                        .has_value());
        CHECK_FALSE(scanScriptEffectVisibility("if (thisLayer.getEffect(0).visible !== true) {}")
                        .has_value());
    }

    TEST_CASE("a spaced visible write still counts") {
        auto t = scanScriptEffectVisibility("thisLayer.getEffect(0).visible   =  false;");
        CHECK(t.has_value());
    }

    TEST_CASE("getEffect on a dynamically fetched layer flags the scan unresolved") {
        auto t = scanScriptEffectVisibility(
            "var l = thisScene.getLayer(name); l.getEffect(0).visible = false;");
        REQUIRE(t.has_value());
        CHECK(t->unresolved);
        CHECK_FALSE(t->ownLayer);
    }

    TEST_CASE("a layer reached through its parent is unresolved") {
        auto t = scanScriptEffectVisibility(
            "var p = thisLayer.getParent(); p.getEffect(0).visible = false;");
        REQUIRE(t.has_value());
        CHECK(t->unresolved);
    }

    TEST_CASE("a literal chain next to a dynamic lookup of a variable receiver is unresolved") {
        auto t = scanScriptEffectVisibility(
            "thisScene.getLayer('A').getEffect(0).visible = false;"
            "var l = thisScene.getLayer(pick()); l.getEffect(0).visible = false;");
        REQUIRE(t.has_value());
        CHECK(t->unresolved);
    }

    TEST_CASE(
        "a literal getLayer chain stays resolved in a script that looks layers up dynamically") {
        auto t = scanScriptEffectVisibility("thisScene.getLayer('A').getEffect(0).visible = false;"
                                            "var kids = thisLayer.getChildren();");
        REQUIRE(t.has_value());
        CHECK_FALSE(t->unresolved);
        REQUIRE(t->layerNames.size() == 1u);
        CHECK(t->layerNames[0] == "A");
    }

    TEST_CASE("a call receiver that is not a literal chain is unresolved next to one that is") {
        auto t = scanScriptEffectVisibility("thisScene.getLayer('A').getEffect(0).visible = false;"
                                            "thisLayer.getParent().getEffect(0).visible = false;");
        REQUIRE(t.has_value());
        CHECK(t->unresolved);
    }

    TEST_CASE("a thisLayer receiver next to a dynamic lookup stays resolved") {
        auto t = scanScriptEffectVisibility("thisLayer.getEffect(0).visible = false;"
                                            "var l = thisScene.getLayer(pick()); l.getMaterial();");
        REQUIRE(t.has_value());
        CHECK(t->ownLayer);
        CHECK_FALSE(t->unresolved);
    }

    TEST_CASE("an indexed receiver over children in a dynamic script is unresolved") {
        auto t = scanScriptEffectVisibility(
            "var c = thisLayer.getChildren(); c[0].getEffect(0).visible = false;");
        REQUIRE(t.has_value());
        CHECK(t->unresolved);
    }

    TEST_CASE("an indexed receiver over literal layers names them without going unresolved") {
        auto t = scanScriptEffectVisibility(
            "var ls = [thisScene.getLayer('A')]; ls[0].getEffect(0).visible = false;");
        REQUIRE(t.has_value());
        CHECK_FALSE(t->unresolved);
        CHECK_FALSE(t->ownLayer);
        REQUIRE(t->layerNames.size() == 1);
        CHECK(t->layerNames[0] == "A");
    }

    TEST_CASE("collectScriptEffectTargets records own ids and literal names") {
        auto                  j = json::parse(R"({"objects": [
            {"id": 176, "name": "SceneScript", "visible": {"value": true, "script":
                "thisScene.getLayer('Room interior').getEffect('Radio Display').visible = on;"}},
            {"id": 499, "name": "Room interior", "effects": [{"name": "a"}]},
            {"id": 288, "name": "Pattern", "origin": {"value": "0 0 0", "script":
                "thisLayer.getEffect(0).visible = false;"}}
        ]})");
        ScriptEffectTargetSet set;
        collectScriptEffectTargets(j, set);

        CHECK(set.ids == std::unordered_set<i32> { 288 });
        CHECK(set.names == std::unordered_set<std::string> { "Room interior" });
        CHECK_FALSE(set.fallback);
        CHECK(set.contains(288, "anything"));
        CHECK(set.contains(7, "Room interior"));
        CHECK_FALSE(set.contains(176, "SceneScript"));
        CHECK_FALSE(set.contains(499 + 1, "other"));
    }

    TEST_CASE("an effect's own visible script is a source") {
        auto                  j = json::parse(R"({"objects": [
            {"id": 5, "name": "L", "effects": [
              {"name": "a", "visible": {"value": true, "script":
                "thisLayer.getEffect(1).visible = false;"}},
              {"name": "b"}]}
        ]})");
        ScriptEffectTargetSet set;
        collectScriptEffectTargets(j, set);
        CHECK(set.ids.count(5) == 1u);
    }

    TEST_CASE("an unresolved script turns every layer on") {
        auto                  j = json::parse(R"({"objects": [
            {"id": 1, "name": "S", "visible": {"value": true, "script":
                "var l = thisScene.getLayer(n); l.getEffect(0).visible = false;"}}
        ]})");
        ScriptEffectTargetSet set;
        collectScriptEffectTargets(j, set);
        CHECK(set.fallback);
        CHECK(set.contains(99, "whatever"));
    }

    TEST_CASE("an unresolved script on an object with a string id and null name does not throw") {
        auto                  j = json::parse(R"({"objects": [
            {"id": "x", "name": null, "visible": {"value": true, "script":
                "var l = thisScene.getLayer(n); l.getEffect(0).visible = false;"}}
        ]})");
        ScriptEffectTargetSet set;
        CHECK_NOTHROW(collectScriptEffectTargets(j, set));
        CHECK(set.fallback);
    }

    TEST_CASE("a scene without candidate scripts leaves the set empty") {
        auto                  j = json::parse(R"({"objects": [
            {"id": 1, "name": "S", "visible": {"value": true, "script":
                "export function update(v) { return v; }"}},
            {"id": 2, "name": "T", "effects": [{"name": "a"}]}
        ]})");
        ScriptEffectTargetSet set;
        collectScriptEffectTargets(j, set);
        CHECK(set.ids.empty());
        CHECK(set.names.empty());
        CHECK_FALSE(set.fallback);
        CHECK_FALSE(set.contains(2, "T"));
    }

    TEST_CASE("a scene without an objects array is accepted") {
        ScriptEffectTargetSet set;
        collectScriptEffectTargets(json::object(), set);
        CHECK_FALSE(set.contains(1, "x"));
    }
}
