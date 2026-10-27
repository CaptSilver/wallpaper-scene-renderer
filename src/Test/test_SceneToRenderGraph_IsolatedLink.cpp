#include <doctest.h>

// Parse() followed by sceneToRenderGraph() for a layer that is read through
// `_rt_imageLayerComposite_<id>_a` by an ordinary effect (not a compose
// layer's `dependencies`).  Such a source keeps drawing itself on screen and
// hands its readers a sprite-sized copy of its chain output.  The copy has to
// come from the synthesized terminal's input ping-pong buffer; taking it from
// _rt_default would give readers a full-framebuffer snapshot again.

#include "RenderGraphTestHelpers.hpp"
#include "SceneParseFixtures.hpp"

#include "Audio/SoundManager.h"
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneImageEffectLayer.h"
#include "SpecTexs.hpp"
#include "VulkanRender/CopyPass.hpp"
#include "VulkanRender/CustomShaderPass.hpp"
#include "VulkanRender/SceneToRenderGraph.hpp"
#include "WPSceneParser.hpp"
#include "WPUserProperties.hpp"

#include <memory>
#include <string>

using namespace wallpaper;
using wek::test::ensureGlslangInit;
using wek::test::findCopyPassTo;
using wek::test::findPassForNode;
using wek::test::makeAssetsVfsWith;
using wek::test::topoIndexOf;

namespace
{

constexpr const char* kPlainImage =
    R"("image": "models/_plain.json", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0")";

// A source with `effects` (a JSON array body, possibly empty).
std::string sourceObject(int id, const std::string& effects = "", const std::string& extra = "") {
    return R"({"id": )" + std::to_string(id) + R"(, "name": "src)" + std::to_string(id) + R"(", )" +
           kPlainImage + R"(, "visible": true)" + extra +
           (effects.empty() ? "" : R"(, "effects": [)" + effects + "]") + "}";
}

const char* kTintEffect = R"({"id": 10, "name": "tint", "visible": true,
    "file": "effects/tint.json"})";

// A reader whose tint effect names `src` in a scene.json pass override.
std::string readerObject(int id, int src) {
    return R"({"id": )" + std::to_string(id) + R"(, "name": "reader)" + std::to_string(id) +
           R"(", )" + kPlainImage + R"(, "visible": true, "effects": [
        {"id": 11, "name": "tint", "visible": true, "file": "effects/tint.json",
         "passes": [{"textures": [null, "_rt_imageLayerComposite_)" +
           std::to_string(src) + R"(_a"]}]}]})";
}

std::string plainObject(int id) {
    return R"({"id": )" + std::to_string(id) + R"(, "name": "plain)" + std::to_string(id) +
           R"(", )" + kPlainImage + R"(, "visible": true})";
}

std::shared_ptr<Scene> parseObjects(const std::string& objects,
                                    const std::string& projectJson = "") {
    ensureGlslangInit();
    static const std::string kPrefix = R"({"general": {"clearcolor": "0 0 0",
        "orthogonalprojection": {"width": 1280, "height": 720}}, "objects": [)";
    auto                     vfs     = wek::test::makeIsolatedLinkVfs();
    audio::SoundManager      sm;
    WPUserProperties         props;
    if (! projectJson.empty()) REQUIRE(props.LoadFromProjectJson(projectJson));
    WPSceneParser parser;
    auto          scene = parser.Parse("isolated_link", kPrefix + objects + "]}", *vfs, sm, props);
    REQUIRE(scene != nullptr);
    return scene;
}

SceneImageEffectLayer* effectLayerOf(Scene& scene, i32 id) {
    REQUIRE(scene.nodeEffectLayerMap.count(id) == 1);
    return scene.nodeEffectLayerMap.at(id);
}

SceneNode* nodeById(SceneNode* n, i32 id) {
    if (n->ID() == id) return n;
    for (auto& c : n->GetChildren()) {
        if (auto* hit = nodeById(c.get(), id)) return hit;
    }
    return nullptr;
}

vulkan::CustomShaderPass* passOf(rg::RenderGraph& rgraph, SceneNode* node) {
    auto* pass = findPassForNode(rgraph, node);
    REQUIRE(pass != nullptr);
    return pass;
}

struct SourceBlends {
    BlendMode base;
    BlendMode terminal;
};

// Builds the graph (ResolveEffect assigns the terminal's final blend there) and
// reads the blend of the source's own base pass and of its on-screen terminal.
SourceBlends blendsOf(Scene& scene, i32 id) {
    auto  rgraph = sceneToRenderGraph(scene);
    auto* layer  = effectLayerOf(scene, id);
    REQUIRE(layer->ResolvedLastOutput() != nullptr);
    return { nodeById(scene.sceneGraph.get(), id)->Mesh()->Material()->blenmode,
             layer->ResolvedLastOutput()->Mesh()->Material()->blenmode };
}

} // namespace

TEST_SUITE("SceneToRenderGraph_IsolatedLink") {
    TEST_CASE("isolated source: synth draws _rt_default, link copies the sprite") {
        auto scene  = parseObjects(sourceObject(401, kTintEffect) + "," + readerObject(501, 401));
        auto rgraph = sceneToRenderGraph(*scene);
        CHECK_FALSE(rgraph->HasCycle());

        auto* layer = effectLayerOf(*scene, 401);
        REQUIRE(layer->IsIsolatedLink());
        REQUIRE(layer->EffectCount() == 2u);

        // The synthesized terminal is the on-screen pass, reading the buffer
        // the real effect left its output in.
        auto* synth = passOf(*rgraph, layer->ResolvedLastOutput());
        CHECK(synth->desc().output == SpecTex_Default);
        REQUIRE(! synth->desc().textures.empty());
        CHECK(synth->desc().textures[0] == layer->SecondTarget());
        CHECK(synth->desc().hideable == vulkan::HideableKind::None);

        // Readers' copy is that same sprite-sized buffer, never the framebuffer.
        auto* copy = findCopyPassTo(*rgraph, GenLinkTex(401));
        REQUIRE(copy != nullptr);
        CHECK(copy->desc().src == layer->SecondTarget());
        CHECK(copy->desc().src != SpecTex_Default);

        auto* readerLayer = effectLayerOf(*scene, 501);
        auto* readerPass  = passOf(*rgraph, readerLayer->GetEffect(0)->chain_writer);
        REQUIRE(readerPass->desc().textures.size() > 1);
        CHECK(readerPass->desc().textures[1] == GenLinkTex(401));

        // real effect -> copy -> reader
        auto* effect0 = passOf(*rgraph, layer->GetEffect(0)->chain_writer);
        CHECK(topoIndexOf(*rgraph, effect0) < topoIndexOf(*rgraph, copy));
        CHECK(topoIndexOf(*rgraph, copy) < topoIndexOf(*rgraph, readerPass));
    }

    TEST_CASE("isolated source: z order on screen is preserved") {
        auto scene  = parseObjects(plainObject(300) + "," + sourceObject(401, kTintEffect) + "," +
                                   plainObject(500) + "," + readerObject(501, 401));
        auto rgraph = sceneToRenderGraph(*scene);
        CHECK_FALSE(rgraph->HasCycle());

        auto* layer = effectLayerOf(*scene, 401);
        auto* synth = passOf(*rgraph, layer->ResolvedLastOutput());
        auto* below = passOf(*rgraph, nodeById(scene->sceneGraph.get(), 300));
        auto* above = passOf(*rgraph, nodeById(scene->sceneGraph.get(), 500));
        CHECK(topoIndexOf(*rgraph, below) < topoIndexOf(*rgraph, synth));
        CHECK(topoIndexOf(*rgraph, synth) < topoIndexOf(*rgraph, above));
    }

    TEST_CASE("isolated source: a reader listed before its source does not cycle") {
        auto scene  = parseObjects(readerObject(501, 401) + "," + sourceObject(401, kTintEffect));
        auto rgraph = sceneToRenderGraph(*scene);
        CHECK_FALSE(rgraph->HasCycle());

        auto* readerLayer = effectLayerOf(*scene, 501);
        auto* readerPass  = passOf(*rgraph, readerLayer->GetEffect(0)->chain_writer);
        REQUIRE(readerPass->desc().textures.size() > 1);
        CHECK(readerPass->desc().textures[1] == GenLinkTex(401));
    }

    TEST_CASE("isolated source: a user-hidden last effect becomes Middle and the synth stays "
              "plain") {
        const std::string hiddenTint = R"({"id": 12, "name": "tint2",
            "visible": {"user": "flame", "value": true}, "file": "effects/tint.json"})";
        auto scene  = parseObjects(sourceObject(401, std::string(kTintEffect) + "," + hiddenTint) +
                                       "," + readerObject(501, 401),
                                   R"({"general":{"properties":{"flame":
                                      {"value":false,"type":"bool"}}}})");
        auto rgraph = sceneToRenderGraph(*scene);
        CHECK_FALSE(rgraph->HasCycle());

        auto* layer = effectLayerOf(*scene, 401);
        REQUIRE(layer->EffectCount() == 3u);
        const auto& hidden = passOf(*rgraph, layer->GetEffect(1)->chain_writer)->desc();
        CHECK(hidden.hideable == vulkan::HideableKind::Middle);
        CHECK(hidden.chain_index == 1);
        CHECK(hidden.chain_effects.size() == 3u);
        CHECK(passOf(*rgraph, layer->ResolvedLastOutput())->desc().hideable ==
              vulkan::HideableKind::None);
    }

    TEST_CASE("isolated source: a script-targeted layer's synth is still not hideable") {
        auto scene  = parseObjects(sourceObject(401,
                                                kTintEffect,
                                                R"(, "origin": {"value": "0 0 0", "script":
                "export function update(v) { thisLayer.getEffect(0).visible = false; return v; }"})") +
                                   "," + readerObject(501, 401));
        auto rgraph = sceneToRenderGraph(*scene);

        auto* layer = effectLayerOf(*scene, 401);
        REQUIRE(layer->EffectCount() == 2u);
        CHECK(passOf(*rgraph, layer->GetEffect(0)->chain_writer)->desc().hideable ==
              vulkan::HideableKind::Middle);
        CHECK(passOf(*rgraph, layer->ResolvedLastOutput())->desc().hideable ==
              vulkan::HideableKind::None);
    }

    TEST_CASE("a bare isolated source: base pass into the ping-pong, synth to screen") {
        auto scene  = parseObjects(sourceObject(401) + "," + readerObject(501, 401));
        auto rgraph = sceneToRenderGraph(*scene);
        CHECK_FALSE(rgraph->HasCycle());

        auto* layer = effectLayerOf(*scene, 401);
        REQUIRE(layer->EffectCount() == 1u);
        auto* base = passOf(*rgraph, nodeById(scene->sceneGraph.get(), 401));
        CHECK(base->desc().output == layer->FirstTarget());

        auto* synth = passOf(*rgraph, layer->ResolvedLastOutput());
        CHECK(synth->desc().output == SpecTex_Default);
        REQUIRE(! synth->desc().textures.empty());
        CHECK(synth->desc().textures[0] == layer->FirstTarget());

        auto* copy = findCopyPassTo(*rgraph, GenLinkTex(401));
        REQUIRE(copy != nullptr);
        CHECK(copy->desc().src == layer->FirstTarget());
    }

    TEST_CASE("a colorBlendMode source: its own blend passthrough is the terminal, readers copy "
              "its input") {
        auto scene  = parseObjects(sourceObject(401, "", R"(, "colorBlendMode": 21)") + "," +
                                   readerObject(501, 401));
        auto rgraph = sceneToRenderGraph(*scene);
        CHECK_FALSE(rgraph->HasCycle());

        auto* layer = effectLayerOf(*scene, 401);
        REQUIRE(layer->IsIsolatedLink());
        REQUIRE(layer->EffectCount() == 1u);

        // The blend passthrough draws on screen with the layer's own camera, not the
        // effect camera a middle pass would get.
        auto* terminalNode = layer->ResolvedLastOutput();
        auto* terminal     = passOf(*rgraph, terminalNode);
        CHECK(terminal->desc().output == SpecTex_Default);
        CHECK(terminalNode->Camera() == layer->FinalCamera());
        REQUIRE(! terminal->desc().textures.empty());
        CHECK(terminal->desc().textures[0] == layer->FirstTarget());

        // Readers get the pre-blend sprite, never the framebuffer.
        auto* copy = findCopyPassTo(*rgraph, GenLinkTex(401));
        REQUIRE(copy != nullptr);
        CHECK(copy->desc().src == layer->FirstTarget());
        CHECK(copy->desc().src != SpecTex_Default);
    }

    // A bare source's base pass fills a cleared ping-pong buffer.  Translucent
    // there stores colour already weighted by alpha, and the synthesized
    // terminal's own alpha blend then weights it again (alpha squared).  A
    // straight write keeps the pair equal to the old single direct draw.
    TEST_CASE("a bare isolated source: the base pass writes straight, the terminal blends") {
        auto scene =
            parseObjects(sourceObject(401, "", R"(, "alpha": 0.5)") + "," + readerObject(501, 401));
        const auto b = blendsOf(*scene, 401);
        CHECK(b.base == BlendMode::Normal);
        CHECK(b.terminal == BlendMode::Translucent);
    }

    TEST_CASE("a bare additive isolated source: straight base write, additive terminal") {
        std::string src = sourceObject(401);
        src.replace(src.find("models/_plain.json"), 18, "models/_add.json");
        auto       scene = parseObjects(src + "," + readerObject(501, 401));
        const auto b     = blendsOf(*scene, 401);
        CHECK(b.base == BlendMode::Normal);
        CHECK(b.terminal == BlendMode::Additive);
    }

    TEST_CASE("a bare screen-blend isolated source: straight base write, screen terminal") {
        auto       scene = parseObjects(sourceObject(401, "", R"(, "colorBlendMode": 7)") + "," +
                                        readerObject(501, 401));
        const auto b     = blendsOf(*scene, 401);
        CHECK(b.base == BlendMode::Normal);
        CHECK(b.terminal == BlendMode::Opaque);
    }

    // Layers with real effects keep the base blend they always had.
    TEST_CASE("an isolated source with an effect keeps its translucent base pass") {
        auto scene   = parseObjects(sourceObject(401, kTintEffect) + "," + readerObject(501, 401));
        const auto b = blendsOf(*scene, 401);
        CHECK(b.base == BlendMode::Translucent);
        CHECK(b.terminal == BlendMode::Translucent);
    }

    TEST_CASE("a compose dependency's graph is unchanged: offscreen sprite, no synth") {
        // 401 is listed by the compose layer AND read by an ordinary reader.
        auto scene =
            parseObjects(sourceObject(401) + "," + readerObject(501, 401) + "," +
                         R"({"id": 402, "name": "compose", "image": "models/util/composelayer.json",
                "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true,
                "dependencies": [401]})");
        auto rgraph = sceneToRenderGraph(*scene);
        CHECK_FALSE(rgraph->HasCycle());

        CHECK(scene->nodeEffectLayerMap.count(401) == 0);
        auto* dep = passOf(*rgraph, nodeById(scene->sceneGraph.get(), 401));
        CHECK(dep->desc().output == GenOffscreenRT(401));
        auto* copy = findCopyPassTo(*rgraph, GenLinkTex(401));
        REQUIRE(copy != nullptr);
        CHECK(copy->desc().src == GenOffscreenRT(401));
    }
}
