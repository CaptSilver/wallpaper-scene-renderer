#include <doctest.h>

#include "ScriptEffectVisResolve.hpp"

#include "Scene/SceneImageEffectLayer.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneShader.h"

using namespace wallpaper;

namespace
{

std::shared_ptr<SceneNode> makeNodeWithCodes(std::vector<ShaderCode> codes) {
    auto          node = std::make_shared<SceneNode>();
    auto          mesh = std::make_shared<SceneMesh>();
    SceneMaterial mat;
    mat.customShader.shader        = std::make_shared<SceneShader>();
    mat.customShader.shader->codes = std::move(codes);
    mesh->AddMaterial(std::move(mat));
    node->AddMesh(mesh);
    return node;
}

std::shared_ptr<SceneImageEffect> makeEffectWithNode(std::string                name,
                                                     std::shared_ptr<SceneNode> n) {
    auto eff  = std::make_shared<SceneImageEffect>();
    eff->name = std::move(name);
    eff->nodes.push_back({ "_rt_effect_pingpong_b_", std::move(n) });
    return eff;
}

ShaderCode goodCode() { return { 0x07230203 }; }

} // namespace

TEST_SUITE("ScriptEffectVisResolve") {
    TEST_CASE("a compile-failure compaction shifts the bound effect's recorded effectIndex to "
              "match") {
        auto                  worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), "_rt_a", "_rt_b");
        layer.AddEffect(makeEffectWithNode("pulse", makeNodeWithCodes({ goodCode() })));
        auto broken =
            makeEffectWithNode("broken", makeNodeWithCodes({})); // empty codes = failed compile
        layer.AddEffect(broken);
        auto bound = makeEffectWithNode("bound", makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(bound);
        // A fourth, always-present effect keeps "bound" from landing last after
        // compaction -- this test is about index correctness, not the separate
        // last-effect exclusion (covered by the next case).
        layer.AddEffect(makeEffectWithNode("trailer", makeNodeWithCodes({ goodCode() })));
        REQUIRE(layer.EffectCount() == 4);
        REQUIRE(layer.RemoveFailedEffects() == 1); // drops "broken" at index 1
        REQUIRE(layer.EffectCount() == 3);
        REQUIRE(layer.GetEffect(1).get() == bound.get()); // "bound" is now at index 1, was 2

        std::unordered_map<i32, SceneImageEffectLayer*> nodeMap { { 42, &layer } };
        std::vector<wek::PendingScriptEffectVis>        pending {
            { bound.get(),
              42,
              "bound_layer",
              "export function mediaThumbnailChanged(e){}",
              "",
              true }
        };
        std::vector<ScenePropertyScript> out;

        wek::resolvePendingScriptEffectVis(pending, nodeMap, out);

        REQUIRE(out.size() == 1);
        CHECK(out[0].effectIndex == 1); // NOT 2 -- the pre-compaction position
        CHECK(out[0].id == 42);
    }

    TEST_CASE("an effect that ends up last on its layer now registers normally") {
        // CustomShaderPass draws a passthrough pipeline for a hidden terminal
        // (is_terminal_hideable) instead of forcing its structural write
        // visible, so the terminal position no longer needs the special-cased
        // skip -- it resolves exactly like any other bound effect.
        auto                  worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), "_rt_a", "_rt_b");
        layer.AddEffect(makeEffectWithNode("pulse", makeNodeWithCodes({ goodCode() })));
        auto bound = makeEffectWithNode("bound", makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(bound); // "bound" is index 1 -- the last one on this layer

        std::unordered_map<i32, SceneImageEffectLayer*> nodeMap { { 42, &layer } };
        std::vector<wek::PendingScriptEffectVis>        pending {
            { bound.get(),
              42,
              "bound_layer",
              "export function mediaThumbnailChanged(e){}",
              "",
              true }
        };
        std::vector<ScenePropertyScript> out;

        wek::resolvePendingScriptEffectVis(pending, nodeMap, out);

        REQUIRE(out.size() == 1);
        CHECK(out[0].id == 42);
        CHECK(out[0].effectIndex == 1);
        CHECK(out[0].attachment == ScenePropertyScript::Attachment::Effect);
    }

    TEST_CASE("a single-effect layer's one effect registers at index 0") {
        // The dominant real-world shape (the "Blurred Cover" idiom): the
        // terminal and effect 0 are the same effect, so there's no
        // predecessor at all -- the passthrough must fall back to the
        // layer's own base image rather than sampling a buffer no earlier
        // effect ever wrote.
        auto                  worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), "_rt_a", "_rt_b");
        auto bound = makeEffectWithNode("bound", makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(bound);
        REQUIRE(layer.EffectCount() == 1u);

        std::unordered_map<i32, SceneImageEffectLayer*> nodeMap { { 99, &layer } };
        std::vector<wek::PendingScriptEffectVis>        pending {
            { bound.get(),
              99,
              "solo_layer",
              "export function mediaThumbnailChanged(e){}",
              "",
              true }
        };
        std::vector<ScenePropertyScript> out;

        wek::resolvePendingScriptEffectVis(pending, nodeMap, out);

        REQUIRE(out.size() == 1);
        CHECK(out[0].id == 99);
        CHECK(out[0].effectIndex == 0);
    }

    TEST_CASE("an effect resolved on identity keeps the right script string and layerName") {
        auto                  worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), "_rt_a", "_rt_b");
        layer.AddEffect(makeEffectWithNode("Defualt", makeNodeWithCodes({ goodCode() })));
        auto current = makeEffectWithNode("Current", makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(current);
        layer.AddEffect(makeEffectWithNode("Previous", makeNodeWithCodes({ goodCode() })));

        std::unordered_map<i32, SceneImageEffectLayer*> nodeMap { { 174, &layer } };
        std::vector<wek::PendingScriptEffectVis>        pending {
            { current.get(),
              174,
              "Album Cover",
              "export function mediaThumbnailChanged(event) { thisObject.visible = "
              "event.hasThumbnail; }",
              R"({"speed":{"value":2}})",
              true }
        };
        std::vector<ScenePropertyScript> out;

        wek::resolvePendingScriptEffectVis(pending, nodeMap, out);

        REQUIRE(out.size() == 1);
        const auto& sps = out[0];
        CHECK(sps.id == 174);
        CHECK(sps.property == "visible");
        CHECK(sps.script == "export function mediaThumbnailChanged(event) { thisObject.visible = "
                            "event.hasThumbnail; }");
        CHECK(sps.scriptProperties == R"({"speed":{"value":2}})");
        CHECK(sps.layerName == "Album Cover");
        CHECK(sps.initialVisible == true);
        CHECK(sps.attachment == ScenePropertyScript::Attachment::Effect);
        CHECK(sps.effectIndex == 1); // "Current" is index 1 -- "Defualt" anchors, not terminal
    }
}
