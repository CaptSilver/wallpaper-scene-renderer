#include <doctest.h>

#include "Scene/Scene.h"
#include "Scene/SceneMaterial.h"

// activeUserPropBindingNames is defined in SceneWallpaper.cpp, next to the
// loadScene() call site that uses it to write the "<id>_bindings.json"
// sidecar the settings panel reads for its gray-out affordance.  MainHandler
// (loadScene's owning class) is declared inside that .cpp with no header, so
// it can't be constructed here -- this one piece of the write is a free
// function precisely so it can be exercised without the rest of it.
namespace wallpaper
{
std::set<std::string> activeUserPropBindingNames(const std::set<std::string>& allPropNames,
                                                 const Scene&                 scene);
}

using namespace wallpaper;

namespace
{

std::set<std::string> allNames {
    "boundUniform", "boundVis", "boundEffectVis", "boundPointsize", "orphan"
};

} // namespace

// A wallpaper's project.json commonly declares properties nothing in that
// wallpaper's own scene.json ever reads (shared template leftovers, or -- per
// GitHub issue reports -- a Now Playing toggle whose only handler was never
// wired up).  The settings panel grays a row out when its name is missing
// from the bindings file; the point of this function is to keep genuinely
// wired properties out of the gray zone while actually excluding the rest,
// rather than declaring everything bound.
TEST_SUITE("active user-property binding names") {
    TEST_CASE("a name absent from every binding table is excluded") {
        Scene scene;
        auto  active = activeUserPropBindingNames(allNames, scene);
        CHECK(active.count("orphan") == 0);
    }

    TEST_CASE("a shader-uniform-bound name is included") {
        Scene         scene;
        SceneMaterial mat;
        scene.userPropUniformBindings["boundUniform"].push_back({ &mat, "g_Color" });

        auto active = activeUserPropBindingNames(allNames, scene);
        CHECK(active.count("boundUniform") == 1);
    }

    TEST_CASE("an object-visibility-bound name is included") {
        Scene scene;
        scene.userPropVisBindings["boundVis"].push_back({ nullptr, true, "", "{}" });

        auto active = activeUserPropBindingNames(allNames, scene);
        CHECK(active.count("boundVis") == 1);
    }

    TEST_CASE("an effect-visibility-bound name is included") {
        Scene scene;
        scene.userPropEffectVisBindings["boundEffectVis"].push_back({ 1, 0, true, "{}" });

        auto active = activeUserPropBindingNames(allNames, scene);
        CHECK(active.count("boundEffectVis") == 1);
    }

    TEST_CASE("a text-pointsize-bound name is included") {
        Scene         scene;
        TextLayerInfo text;
        text.pointsizeUserProp = "boundPointsize";
        scene.textLayers.push_back(text);

        auto active = activeUserPropBindingNames(allNames, scene);
        CHECK(active.count("boundPointsize") == 1);
    }

    TEST_CASE("only names present in the candidate set are ever returned") {
        // A binding on a name the caller didn't ask about (e.g. a property
        // belonging to a different, already-unloaded scene) must not leak
        // into the result.
        Scene         scene;
        SceneMaterial mat;
        scene.userPropUniformBindings["notInCandidateSet"].push_back({ &mat, "g_Color" });

        auto active = activeUserPropBindingNames(allNames, scene);
        CHECK(active.count("notInCandidateSet") == 0);
    }

    TEST_CASE("an all-orphan candidate set against a scene with no bindings is empty") {
        Scene scene;
        auto  active = activeUserPropBindingNames({ "a", "b", "c" }, scene);
        CHECK(active.empty());
    }

    // A property with none of the four direct bindings above can still be
    // read by a property script -- e.g. a Now Playing toggle whose only
    // effect is `if (scriptProperties.enableplayer) { ... }` inside a script
    // body, with no visible/uniform field pointed at it in scene.json.  Those
    // properties do something; they should not gray out.
    TEST_CASE("a name referenced via a script's structured user binding is included") {
        Scene               scene;
        ScenePropertyScript sps {};
        sps.id               = 1;
        sps.property         = "alpha";
        sps.script           = "function update(){ return scriptProperties.enableplayer; }";
        sps.scriptProperties = R"({"enableplayer":{"user":"scriptBound","value":true}})";
        scene.propertyScripts.push_back(sps);

        auto active = activeUserPropBindingNames({ "scriptBound" }, scene);
        CHECK(active.count("scriptBound") == 1);
    }

    TEST_CASE("a name referenced only in a property script's body text is included") {
        Scene               scene;
        ScenePropertyScript sps {};
        sps.id       = 2;
        sps.property = "visible";
        sps.script   = "function update(){ return engine.userProperties.textOnlyBound; }";
        scene.propertyScripts.push_back(sps);

        auto active = activeUserPropBindingNames({ "textOnlyBound" }, scene);
        CHECK(active.count("textOnlyBound") == 1);
    }

    TEST_CASE("a name referenced via a color script's structured user binding is included") {
        Scene            scene;
        SceneMaterial    mat;
        SceneColorScript csi {};
        csi.id               = 3;
        csi.material         = &mat;
        csi.script           = "function update(){ return WEColor.WHITE; }";
        csi.scriptProperties = R"({"tint":{"user":"colorScriptBound"}})";
        scene.colorScripts.push_back(csi);

        auto active = activeUserPropBindingNames({ "colorScriptBound" }, scene);
        CHECK(active.count("colorScriptBound") == 1);
    }

    TEST_CASE("a name referenced via a shader-value script's structured user binding is included") {
        Scene                  scene;
        SceneShaderValueScript sv {};
        sv.id               = 4;
        sv.uniformName      = "g_Foo";
        sv.script           = "function update(v){ return v; }";
        sv.scriptProperties = R"({"foo":{"user":"shaderScriptBound"}})";
        scene.shaderValueScripts.push_back(sv);

        auto active = activeUserPropBindingNames({ "shaderScriptBound" }, scene);
        CHECK(active.count("shaderScriptBound") == 1);
    }

    TEST_CASE("a name referenced only in a sound-volume script's body text is included") {
        Scene                    scene;
        Scene::SoundVolumeScript svs {};
        svs.script    = "engine.userProperties.soundScriptBound";
        svs.layerName = "music";
        scene.soundVolumeScripts.push_back(svs);

        auto active = activeUserPropBindingNames({ "soundScriptBound" }, scene);
        CHECK(active.count("soundScriptBound") == 1);
    }

    TEST_CASE("a name referenced only in a text layer's script body is included") {
        Scene         scene;
        TextLayerInfo text {};
        text.script = "engine.userProperties['textLayerBound']";
        scene.textLayers.push_back(text);

        auto active = activeUserPropBindingNames({ "textLayerBound" }, scene);
        CHECK(active.count("textLayerBound") == 1);
    }

    TEST_CASE("a name is excluded when scripts exist but none of them mention it") {
        Scene               scene;
        ScenePropertyScript sps {};
        sps.id               = 5;
        sps.property         = "alpha";
        sps.script           = "function update(){ return 1; }";
        sps.scriptProperties = R"({"other":{"user":"someOtherProp"}})";
        scene.propertyScripts.push_back(sps);

        auto active = activeUserPropBindingNames({ "neverMentioned" }, scene);
        CHECK(active.count("neverMentioned") == 0);
    }
}
