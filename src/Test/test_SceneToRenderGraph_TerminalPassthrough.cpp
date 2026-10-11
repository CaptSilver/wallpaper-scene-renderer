#include <doctest.h>

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

// Covers the wiring SceneToRenderGraph::LoadEffectChain/ToGraphPass do for a
// terminal-hideable pass: both of the chain's physical ping-pong buffers get
// registered as reads (not just whichever one the terminal's own material
// happens to declare), and the terminal's Desc carries one representative
// node per predecessor effect for CustomShaderPass::SelectPassthroughSource
// to read every frame. See test_CustomShaderPassDesc.cpp for the predicate
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

vulkan::CustomShaderPass* findPassForNode(rg::RenderGraph& rgraph, SceneNode* node) {
    for (auto id : rgraph.topologicalOrder()) {
        auto* p = dynamic_cast<vulkan::CustomShaderPass*>(rgraph.getPass(id));
        if (p != nullptr && p->desc().node == node) return p;
    }
    return nullptr;
}

// Populates `scene` with one world node (id `worldId`) whose camera owns an
// image-effect chain with `effectNames.size()` effects, one node each, in
// order.  Registers a script-attached visibility binding on the terminal
// (last) effect so sceneToRenderGraph marks it hideable, exactly
// as a real "visible": {"script": ...} on the layer's last effect would.
// Takes `scene` by reference (rather than owning one) because Scene is
// NoCopy/NoMove -- a fixture struct can hold the shared_ptrs this returns,
// not the Scene itself.
struct Fixture {
    std::shared_ptr<SceneNode>              worldNode;
    std::vector<std::shared_ptr<SceneNode>> effectNodes;
    std::shared_ptr<SceneImageEffectLayer>  layer;
};

Fixture makeHideableTerminalChain(Scene& scene, i32 worldId,
                                  const std::vector<std::string>& effectNames) {
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

    ScenePropertyScript ps;
    ps.id          = worldId;
    ps.attachment  = ScenePropertyScript::Attachment::Effect;
    ps.effectIndex = (i32)(effectNames.size() - 1); // the layer's actual terminal index
    scene.propertyScripts.push_back(ps);

    return f;
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

        REQUIRE(desc.passthrough_predecessor_nodes.size() == 2u);
        CHECK(desc.passthrough_predecessor_nodes[0] == f.effectNodes[0].get()); // Defualt
        CHECK(desc.passthrough_predecessor_nodes[1] == f.effectNodes[1].get()); // Current

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
        CHECK(desc.passthrough_predecessor_nodes.empty());
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
        CHECK(desc.passthrough_predecessor_nodes.empty());
    }
}
