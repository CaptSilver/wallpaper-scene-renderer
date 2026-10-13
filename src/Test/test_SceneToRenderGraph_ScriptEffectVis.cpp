#include <doctest.h>

// Parse() followed by sceneToRenderGraph(): which effect passes of a parsed
// scene are hideable.  A SceneScript can hide any built effect at runtime with
// getEffect(...).visible, bound to a property or not, so the layers such a
// script reaches must have every effect tagged hideable or the hidden pass
// keeps its buffer clear and blanks the layer.  Layers no script touches must
// keep ordinary passes, so the graph of an ordinary scene does not change.

#include "RenderGraphTestHelpers.hpp"
#include "SceneParseFixtures.hpp"

#include "Audio/SoundManager.h"
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneImageEffectLayer.h"
#include "VulkanRender/CustomShaderPass.hpp"
#include "VulkanRender/SceneToRenderGraph.hpp"
#include "WPSceneParser.hpp"
#include "WPUserProperties.hpp"

#include <memory>
#include <string>

using namespace wallpaper;
using wek::test::ensureGlslangInit;
using wek::test::findPassForNode;
using wek::test::kEffectFileJson;
using wek::test::makeAssetsVfsWith;

namespace
{

// `names.size()` literal-visible effects, all backed by the same effect file.
std::string effectsJson(std::initializer_list<const char*> names) {
    std::string out = "[";
    int         id  = 10;
    for (const char* n : names) {
        if (id > 10) out += ",";
        out += R"({"id": )" + std::to_string(id++) + R"(, "name": ")" + n +
               R"(", "visible": true, "file": "effects/tint.json"})";
    }
    return out + "]";
}

// An image object with effects; `extra` is spliced in before "effects" and
// must end with a comma when non-empty.
std::string imageObject(int id, const char* name, const std::string& effects,
                        const std::string& extra = "") {
    return R"({"id": )" + std::to_string(id) + R"(, "name": ")" + name + R"(",
        "image": "models/_plain.json", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
        )" +
           extra + R"("effects": )" + effects + "}";
}

std::shared_ptr<Scene> parseObjects(const std::string& objects) {
    ensureGlslangInit();
    static const std::string kPrefix = R"({"general": {"clearcolor": "0 0 0",
        "orthogonalprojection": {"width": 1280, "height": 720}}, "objects": [)";
    auto                     vfs     = makeAssetsVfsWith({
        { "/effects/tint.json", kEffectFileJson },
        // A compose layer with no authored effects gets a synthesized passthrough effect.
        { "/models/util/composelayer.json", wek::test::kPlainImageJson },
        { "/materials/util/effectpassthrough.json", wek::test::kPlainMaterialJson },
    });
    audio::SoundManager      sm;
    WPUserProperties         props;
    WPSceneParser            parser;
    auto scene = parser.Parse("script_effect_vis", kPrefix + objects + "]}", *vfs, sm, props);
    REQUIRE(scene != nullptr);
    return scene;
}

SceneImageEffectLayer* effectLayerOf(Scene& scene, i32 id) {
    REQUIRE(scene.nodeEffectLayerMap.count(id) == 1);
    return scene.nodeEffectLayerMap.at(id);
}

const vulkan::CustomShaderPass::Desc& writerDesc(rg::RenderGraph& rgraph, SceneImageEffectLayer* l,
                                                 usize effect) {
    auto* pass = findPassForNode(rgraph, l->GetEffect(effect)->chain_writer);
    REQUIRE(pass != nullptr);
    return pass->desc();
}

} // namespace

TEST_SUITE("SceneToRenderGraph_ScriptEffectVis") {
    TEST_CASE("a layer no script touches keeps ordinary passes") {
        auto scene  = parseObjects(imageObject(600, "canvas", effectsJson({ "a", "b" })));
        auto rgraph = sceneToRenderGraph(*scene);

        auto* layer = effectLayerOf(*scene, 600);
        REQUIRE(layer->EffectCount() == 2u);
        for (usize i = 0; i < 2; i++) {
            CHECK(writerDesc(*rgraph, layer, i).hideable == vulkan::HideableKind::None);
        }
    }

    TEST_CASE("a script hiding its own layer's first effect by index makes the chain hideable") {
        auto scene  = parseObjects(imageObject(600,
                                               "canvas",
                                               effectsJson({ "a", "b" }),
                                               R"("origin": {"value": "0 0 0", "script":
                "export function update(v) { thisLayer.getEffect(0).visible = false; return v; }"},)"));
        auto rgraph = sceneToRenderGraph(*scene);

        auto* layer = effectLayerOf(*scene, 600);
        REQUIRE(layer->EffectCount() == 2u);

        const auto& first = writerDesc(*rgraph, layer, 0);
        CHECK(first.hideable == vulkan::HideableKind::Middle);
        CHECK(first.chain_index == 0);
        REQUIRE(first.passthrough_input_tex_index_a >= 0);
        CHECK(first.textures[(usize)first.passthrough_input_tex_index_a] == layer->FirstTarget());

        const auto& last = writerDesc(*rgraph, layer, 1);
        CHECK(last.hideable == vulkan::HideableKind::Terminal);
        CHECK(last.chain_effects.size() == 2u);
    }

    TEST_CASE("a script on another object hiding an effect by layer and effect name makes that "
              "chain hideable") {
        // The host has no effects of its own; it names the target layer and effect with string
        // literals, so only the named layer is reached.
        const std::string host  = imageObject(176,
                                              "SceneScript",
                                              "[]",
                                              R"("visible": {"value": false, "script":
                "export function update(v) { thisScene.getLayer('Room interior').getEffect('Radio Display').visible = false; return v; }"},)");
        auto              scene = parseObjects(
            host + "," + imageObject(499, "Room interior", effectsJson({ "a", "b", "c" })) + "," +
            imageObject(700, "bystander", effectsJson({ "x", "y" })));
        auto rgraph = sceneToRenderGraph(*scene);

        auto* room = effectLayerOf(*scene, 499);
        REQUIRE(room->EffectCount() == 3u);
        for (usize i = 0; i < 2; i++) {
            const auto& d = writerDesc(*rgraph, room, i);
            CHECK(d.hideable == vulkan::HideableKind::Middle);
            CHECK(d.chain_index == (i32)i);
            REQUIRE(d.passthrough_input_tex_index_a >= 0);
            CHECK(d.textures[(usize)d.passthrough_input_tex_index_a] ==
                  (i == 0 ? room->FirstTarget() : room->SecondTarget()));
        }
        CHECK(writerDesc(*rgraph, room, 2).hideable == vulkan::HideableKind::Terminal);

        // Per layer, not per scene: an object no script reaches stays ordinary.
        auto* bystander = effectLayerOf(*scene, 700);
        REQUIRE(bystander->EffectCount() == 2u);
        CHECK(writerDesc(*rgraph, bystander, 0).hideable == vulkan::HideableKind::None);
        CHECK(writerDesc(*rgraph, bystander, 1).hideable == vulkan::HideableKind::None);
    }

    TEST_CASE("every layer sharing the name a script targets is tagged") {
        const std::string host = imageObject(2300,
                                             "SceneScript",
                                             "[]",
                                             R"("visible": {"value": false, "script":
                "export function update(v) { thisScene.getLayer('playervolume').getEffect('b').visible = false; return v; }"},)");
        auto              scene =
            parseObjects(host + "," + imageObject(2200, "playervolume", effectsJson({ "a", "b" })) +
                         "," + imageObject(2215, "playervolume", effectsJson({ "a", "b" })));
        auto rgraph = sceneToRenderGraph(*scene);

        for (i32 id : { 2200, 2215 }) {
            auto* layer = effectLayerOf(*scene, id);
            REQUIRE(layer->EffectCount() == 2u);
            CHECK(writerDesc(*rgraph, layer, 0).hideable == vulkan::HideableKind::Middle);
            CHECK(writerDesc(*rgraph, layer, 1).hideable == vulkan::HideableKind::Terminal);
        }
    }

    TEST_CASE("a compose host whose script only names another layer keeps its synthesized "
              "passthrough ordinary") {
        // The host's own pass is the compose passthrough, not an effect any script reaches, so
        // tagging it would change the graph of a layer nothing toggles.
        const std::string host = R"({"id": 176, "name": "SceneScript",
            "image": "models/util/composelayer.json", "origin": "0 0 0", "scale": "1 1 1",
            "angles": "0 0 0", "visible": {"value": true, "script":
                "export function update(v) { thisScene.getLayer('Room interior').getEffect('a').visible = false; return v; }"}})";
        auto              scene =
            parseObjects(host + "," + imageObject(499, "Room interior", effectsJson({ "a", "b" })));
        auto rgraph = sceneToRenderGraph(*scene);

        auto* hostLayer = effectLayerOf(*scene, 176);
        REQUIRE(hostLayer->EffectCount() == 1u);
        CHECK(writerDesc(*rgraph, hostLayer, 0).hideable == vulkan::HideableKind::None);
    }
    TEST_CASE("a script-targeted compose layer with only a hidden effect ends in a hidden terminal "
              "instead of a synthesized passthrough") {
        // The authored effect now counts as built, so no passthrough is synthesized for the
        // compose layer; the chain's last pass is the hidden effect, and copybackground decides
        // whether the base image is considered drawn under it.
        for (const bool copyBackground : { true, false }) {
            const std::string obj    = std::string(R"({"id": 176, "name": "glass",
                "image": "models/util/composelayer.json", "origin": "0 0 0", "scale": "1 1 1",
                "angles": "0 0 0", "copybackground": )") +
                                       (copyBackground ? "true" : "false") +
                                       R"(, "visible": {"value": true, "script":
                "export function update(v) { thisLayer.getEffect(0).visible = true; return v; }"},
                "effects": [{"id": 10, "name": "a", "visible": false,
                             "file": "effects/tint.json"}]})";
            auto              scene  = parseObjects(obj);
            auto              rgraph = sceneToRenderGraph(*scene);

            auto* layer = effectLayerOf(*scene, 176);
            REQUIRE(layer->EffectCount() == 1u);
            CHECK_FALSE(layer->GetEffect(0)->visible);
            const auto& d = writerDesc(*rgraph, layer, 0);
            CHECK(d.hideable == vulkan::HideableKind::Terminal);
            CHECK(d.chain_base_image_written == copyBackground);
        }
    }
}
