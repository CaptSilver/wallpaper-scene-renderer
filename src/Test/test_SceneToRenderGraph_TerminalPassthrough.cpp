#include <doctest.h>

#include "RenderGraphTestHelpers.hpp"
#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/CustomShaderPass.hpp"
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneImageEffectLayer.h"

#include <memory>
#include <string>
#include <vector>

using namespace wallpaper;
using wek::test::findPassForNode;

// Covers the wiring SceneToRenderGraph::LoadEffectChain/ToGraphPass do for a
// terminal-hideable pass: both of the chain's physical ping-pong buffers get
// registered as reads (not just whichever one the terminal's own material
// happens to declare), and the terminal's Desc carries the whole effect chain
// and its own index for CustomShaderPass::SelectPassthroughSource to read
// every frame. See test_CustomShaderPassDesc.cpp for the predicate
// itself.
namespace
{

std::shared_ptr<SceneNode> makeMaterialNode(std::string name) {
    auto          node = std::make_shared<SceneNode>();
    auto          mesh = std::make_shared<SceneMesh>();
    SceneMaterial mat;
    mat.name = std::move(name);
    mesh->AddMaterial(std::move(mat));
    node->AddMesh(mesh);
    return node;
}

// Matches ResolveEffect's own expectation (SceneImageEffectLayer.cpp): a
// node's `output` must start with the ping-pong-B prefix (or be
// SpecTex_Default) for the resolve loop to recognise it as a chain step and
// track it as a last_output candidate.
std::shared_ptr<SceneImageEffect> makeEffect(std::string name, std::shared_ptr<SceneNode> node) {
    auto eff  = std::make_shared<SceneImageEffect>();
    eff->name = std::move(name);
    eff->nodes.push_back({ "_rt_effect_pingpong_b_", std::move(node) });
    return eff;
}

// Populates `scene` with one world node (id `worldId`) whose camera owns an
// image-effect chain with `effectNames.size()` effects, one node each, in
// order.  Registers a script-attached visibility binding on each effect index
// in `boundIndices` so sceneToRenderGraph marks it hideable, exactly as a real
// "visible": {"script": ...} on that effect would.
// Takes `scene` by reference (rather than owning one) because Scene is
// NoCopy/NoMove -- a fixture struct can hold the shared_ptrs this returns,
// not the Scene itself.
struct Fixture {
    std::shared_ptr<SceneNode>              worldNode;
    std::vector<std::shared_ptr<SceneNode>> effectNodes;
    std::shared_ptr<SceneImageEffectLayer>  layer;
};

Fixture makeChain(Scene& scene, i32 worldId, const std::vector<std::string>& effectNames,
                  const std::vector<i32>& boundIndices) {
    Fixture f;
    scene.cameras["effect"]    = std::make_shared<SceneCamera>(2, 2, 0.1f, 100.0f);
    scene.cameras["world_cam"] = std::make_shared<SceneCamera>(2, 2, 0.1f, 100.0f);

    f.worldNode       = makeMaterialNode("world");
    f.worldNode->ID() = worldId;
    f.worldNode->SetCamera("world_cam");
    scene.sceneGraph->AppendChild(f.worldNode);

    f.layer =
        std::make_shared<SceneImageEffectLayer>(f.worldNode.get(),
                                                "_rt_effect_pingpong_a_" + std::to_string(worldId),
                                                "_rt_effect_pingpong_b_" + std::to_string(worldId));

    for (auto& name : effectNames) {
        auto node = makeMaterialNode(name);
        f.effectNodes.push_back(node);
        f.layer->AddEffect(makeEffect(name, node));
    }
    scene.cameras.at("world_cam")->AttatchImgEffect(f.layer);

    for (i32 idx : boundIndices) {
        ScenePropertyScript ps;
        ps.id          = worldId;
        ps.attachment  = ScenePropertyScript::Attachment::Effect;
        ps.effectIndex = idx;
        scene.propertyScripts.push_back(ps);
    }

    return f;
}

Fixture makeHideableTerminalChain(Scene& scene, i32 worldId,
                                  const std::vector<std::string>& effectNames) {
    return makeChain(scene, worldId, effectNames, { (i32)effectNames.size() - 1 });
}

} // namespace

TEST_SUITE("SceneToRenderGraph_TerminalPassthrough") {
    TEST_CASE("Album Cover's Defualt/Current/Previous shape: terminal gets both buffers and the "
              "ordered predecessor list") {
        Scene scene;
        auto  f = makeHideableTerminalChain(scene, 174, { "Defualt", "Current", "Previous" });

        auto rgraph = wallpaper::sceneToRenderGraph(scene);

        auto* terminalPass = findPassForNode(*rgraph, f.layer->ResolvedLastOutput());
        REQUIRE(terminalPass != nullptr);
        const auto& desc = terminalPass->desc();
        CHECK(desc.hideable == vulkan::HideableKind::Terminal);

        REQUIRE(desc.chain_effects.size() == 3u);
        CHECK(desc.chain_effects[0] == f.layer->GetEffect(0).get()); // Defualt
        CHECK(desc.chain_effects[1] == f.layer->GetEffect(1).get()); // Current
        CHECK(desc.chain_effects[2] == f.layer->GetEffect(2).get()); // Previous
        CHECK(desc.chain_index == 2);

        CHECK(desc.passthrough_input_tex_index_a >= 0);
        CHECK(desc.passthrough_input_tex_index_b >= 0);
        CHECK(desc.passthrough_input_tex_index_a != desc.passthrough_input_tex_index_b);
        REQUIRE((usize)desc.passthrough_input_tex_index_a < desc.textures.size());
        REQUIRE((usize)desc.passthrough_input_tex_index_b < desc.textures.size());
        // Both of the layer's own two buffer names are reachable this way --
        // not just whichever one the real material's texture list resolved
        // to statically.
        std::vector<std::string> got {
            desc.textures[(usize)desc.passthrough_input_tex_index_a],
            desc.textures[(usize)desc.passthrough_input_tex_index_b],
        };
        CHECK(((got[0] == f.layer->FirstTarget() && got[1] == f.layer->SecondTarget()) ||
               (got[1] == f.layer->FirstTarget() && got[0] == f.layer->SecondTarget())));
    }

    TEST_CASE("single-effect chain: terminal IS effect 0, no predecessors to gather") {
        Scene scene;
        auto  f = makeHideableTerminalChain(scene, 2065, { "blur" });

        auto rgraph = wallpaper::sceneToRenderGraph(scene);

        auto* terminalPass = findPassForNode(*rgraph, f.layer->ResolvedLastOutput());
        REQUIRE(terminalPass != nullptr);
        const auto& desc = terminalPass->desc();
        CHECK(desc.hideable == vulkan::HideableKind::Terminal);
        CHECK(desc.chain_effects.size() == 1u);
        CHECK(desc.chain_index == 0);
        CHECK(desc.passthrough_input_tex_index_a >= 0);
        CHECK(desc.passthrough_input_tex_index_b >= 0);
    }

    TEST_CASE("an ordinary (non-hideable) terminal gets neither buffer index nor predecessors") {
        Scene scene;
        scene.cameras["effect"]    = std::make_shared<SceneCamera>(2, 2, 0.1f, 100.0f);
        scene.cameras["world_cam"] = std::make_shared<SceneCamera>(2, 2, 0.1f, 100.0f);

        auto worldNode  = makeMaterialNode("world");
        worldNode->ID() = 99;
        worldNode->SetCamera("world_cam");
        scene.sceneGraph->AppendChild(worldNode);

        auto layer = std::make_shared<SceneImageEffectLayer>(
            worldNode.get(), "_rt_effect_pingpong_a_99", "_rt_effect_pingpong_b_99");
        auto onlyNode = makeMaterialNode("only");
        layer->AddEffect(makeEffect("only", onlyNode));
        scene.cameras.at("world_cam")->AttatchImgEffect(layer);
        // No ScenePropertyScript/userPropEffectVisBindings entry -- this
        // terminal never registers a runtime binding.

        auto rgraph = wallpaper::sceneToRenderGraph(scene);

        auto* terminalPass = findPassForNode(*rgraph, layer->ResolvedLastOutput());
        REQUIRE(terminalPass != nullptr);
        const auto& desc = terminalPass->desc();
        CHECK(desc.hideable == vulkan::HideableKind::None);
        CHECK(desc.passthrough_input_tex_index_a == -1);
        CHECK(desc.passthrough_input_tex_index_b == -1);
        CHECK(desc.chain_effects.empty());
        CHECK(desc.chain_index == -1);
    }

    TEST_CASE("a bound first effect becomes a middle pass reading the layer image buffer") {
        Scene scene;
        auto  f      = makeChain(scene, 177, { "scroll", "tint" }, { 0 });
        auto  rgraph = wallpaper::sceneToRenderGraph(scene);
        auto* pass   = findPassForNode(*rgraph, f.effectNodes[0].get());
        REQUIRE(pass != nullptr);
        const auto& d = pass->desc();
        CHECK(d.hideable == vulkan::HideableKind::Middle);
        CHECK(d.chain_index == 0);
        CHECK(d.chain_effects.size() == 2u);
        REQUIRE(d.passthrough_input_tex_index_a >= 0);
        CHECK(d.textures[(usize)d.passthrough_input_tex_index_a] == f.layer->FirstTarget());
        CHECK(d.passthrough_input_tex_index_b == -1);
    }

    TEST_CASE("a bound second effect of three reads the second buffer") {
        Scene scene;
        auto  f      = makeChain(scene, 300, { "a", "b", "c" }, { 1 });
        auto  rgraph = wallpaper::sceneToRenderGraph(scene);
        auto* pass   = findPassForNode(*rgraph, f.effectNodes[1].get());
        REQUIRE(pass != nullptr);
        const auto& d = pass->desc();
        CHECK(d.hideable == vulkan::HideableKind::Middle);
        REQUIRE(d.passthrough_input_tex_index_a >= 0);
        CHECK(d.textures[(usize)d.passthrough_input_tex_index_a] == f.layer->SecondTarget());
    }

    TEST_CASE("an unbound effect stays an ordinary pass") {
        Scene scene;
        auto  f      = makeChain(scene, 301, { "a", "b" }, { 1 });
        auto  rgraph = wallpaper::sceneToRenderGraph(scene);
        auto* pass   = findPassForNode(*rgraph, f.effectNodes[0].get());
        REQUIRE(pass != nullptr);
        CHECK(pass->desc().hideable == vulkan::HideableKind::None);
    }

    TEST_CASE("the last effect stays a terminal reading both buffers") {
        Scene scene;
        auto  f      = makeChain(scene, 302, { "a", "b" }, { 0, 1 });
        auto  rgraph = wallpaper::sceneToRenderGraph(scene);
        auto* term   = findPassForNode(*rgraph, f.layer->ResolvedLastOutput());
        REQUIRE(term != nullptr);
        CHECK(term->desc().hideable == vulkan::HideableKind::Terminal);
        CHECK(term->desc().passthrough_input_tex_index_b >= 0);
    }

    TEST_CASE("a compose layer that does not draw its own image marks the base unwritten") {
        Scene scene;
        auto  f = makeChain(scene, 303, { "a", "b" }, { 0 });
        f.layer->SetPassthrough(true);
        f.layer->SetCopyBackground(false);
        auto  rgraph = wallpaper::sceneToRenderGraph(scene);
        auto* pass   = findPassForNode(*rgraph, f.effectNodes[0].get());
        REQUIRE(pass != nullptr);
        CHECK_FALSE(pass->desc().chain_base_image_written);
    }

    TEST_CASE("a bound effect whose terminal write comes from the last effect only") {
        // effect0 bound, effect1 writes only a private FBO: effect0's node is
        // the layer's terminal node and must be the Terminal pass.
        Scene scene;
        auto  f = makeChain(scene, 304, { "a", "b" }, { 0 });
        // Redirect effect1's node to a private offscreen target.
        f.layer->GetEffect(1)->nodes.front().output = "_rt_private_304";
        auto  rgraph                                = wallpaper::sceneToRenderGraph(scene);
        auto* term = findPassForNode(*rgraph, f.layer->ResolvedLastOutput());
        REQUIRE(term != nullptr);
        CHECK(term->desc().hideable == vulkan::HideableKind::Terminal);
        CHECK(term->desc().chain_index == 0);
    }

    TEST_CASE("a multi-node effect tags the ping-pong node as its middle pass") {
        Scene scene;
        auto  f           = makeChain(scene, 305, { "a", "b" }, { 0 });
        auto  privateNode = makeMaterialNode("a_private");
        f.layer->GetEffect(0)->nodes.insert(f.layer->GetEffect(0)->nodes.begin(),
                                            { "_rt_private_305", privateNode });
        auto  rgraph = wallpaper::sceneToRenderGraph(scene);
        auto* priv   = findPassForNode(*rgraph, privateNode.get());
        auto* pp     = findPassForNode(*rgraph, f.effectNodes[0].get());
        REQUIRE(priv != nullptr);
        REQUIRE(pp != nullptr);
        CHECK(priv->desc().hideable == vulkan::HideableKind::None);
        CHECK(pp->desc().hideable == vulkan::HideableKind::Middle);
    }
}
