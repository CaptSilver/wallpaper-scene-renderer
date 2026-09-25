#include <doctest.h>

#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/CustomShaderPass.hpp"
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneMaterial.h"
#include "SpecTexs.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace wallpaper;

namespace
{
// Find the CustomShaderPass whose material name (PassNode::name(), set from
// SceneToRenderGraph's `passName = material->name`) matches, and return the
// texture key bound to `tex_index` -- empty if the slot was never resolved.
// Passes are identified by name rather than a captured NodeID because
// sceneToRenderGraph() builds the whole graph in one call; there is no
// per-node handle for a test to hang onto.
std::string ResolvedTexAt(const rg::RenderGraph& rgraph, std::string_view passName, u32 tex_index) {
    for (auto id : rgraph.topologicalOrder()) {
        auto* passNode = rgraph.getPassNode(id);
        if (passNode == nullptr || passNode->name() != passName) continue;
        auto* csp = dynamic_cast<vulkan::CustomShaderPass*>(rgraph.getPass(id));
        if (csp == nullptr) continue;
        auto& texs = csp->desc().textures;
        if (tex_index >= texs.size()) return "";
        return texs[tex_index];
    }
    return "";
}
} // namespace

TEST_SUITE("SceneToRenderGraph_LinkResolution") {
    TEST_CASE("two consumers of the same link source do not create a cyclic graph") {
        Scene scene;
        auto  addNode =
            [&](i32 id, std::string_view passName, std::vector<std::string> textures = {}) {
                auto          node = std::make_shared<SceneNode>();
                auto          mesh = std::make_shared<SceneMesh>();
                SceneMaterial mat;
                mat.name     = std::string(passName);
                mat.textures = std::move(textures);
                mesh->AddMaterial(std::move(mat));
                node->AddMesh(mesh);
                node->ID() = id;
                scene.sceneGraph->AppendChild(node);
            };

        // Two ordinary on-screen layers drawn in z-order act as the link sources,
        // and one later pass links both of them in a single draw.
        addNode(288, "flatA");
        addNode(536, "flatB");
        addNode(16, "canvas_paint", { GenLinkTex(288), GenLinkTex(536) });
        // A separate, later pass references the first source again.  Each reference
        // used to get its own copy of that one frozen snapshot, and the version bump
        // between the copies closed an ordering loop.
        addNode(17, "canvas_present", { GenLinkTex(288) });

        auto rgraph = wallpaper::sceneToRenderGraph(scene);
        CHECK_FALSE(rgraph->HasCycle());
    }

    TEST_CASE("a layer that reads its own link texture does not create a cyclic graph") {
        Scene scene;
        auto  addNode =
            [&](i32 id, std::string_view passName, std::vector<std::string> textures = {}) {
                auto          node = std::make_shared<SceneNode>();
                auto          mesh = std::make_shared<SceneMesh>();
                SceneMaterial mat;
                mat.name     = std::string(passName);
                mat.textures = std::move(textures);
                mesh->AddMaterial(std::move(mat));
                node->AddMesh(mesh);
                node->ID() = id;
                scene.sceneGraph->AppendChild(node);
            };

        // Chill Time (2925278995) object 18's own composite reads its own id -- both of its
        // crt_scan_line effects resolve to GenLinkTex(18) because the layer's copybackground
        // base texture is a spec tex, so ParseSpecTexName's _b fallback lands on the same
        // self-reference _a already takes.  This node has no camera, so it also covers the
        // no-pre-effect-capture fallback path (a layer with no imgeff, or one whose
        // copybackground=false skipped its base pass).
        addNode(18, "self_reader", { GenLinkTex(18) });

        auto rgraph = wallpaper::sceneToRenderGraph(scene);
        CHECK_FALSE(rgraph->HasCycle());
    }

    TEST_CASE("a two-hop cross-layer link chain resolves both readers to real textures") {
        Scene scene;
        auto  addNode =
            [&](i32 id, std::string_view passName, std::vector<std::string> textures = {}) {
                auto          node = std::make_shared<SceneNode>();
                auto          mesh = std::make_shared<SceneMesh>();
                SceneMaterial mat;
                mat.name     = std::string(passName);
                mat.textures = std::move(textures);
                mesh->AddMaterial(std::move(mat));
                node->AddMesh(mesh);
                node->ID() = id;
                scene.sceneGraph->AppendChild(node);
            };

        // Real-Time Earth's night-lights shape: source (id 424) also samples its
        // own link (the self-reference the other test case covers in isolation);
        // a middle layer (420) links the source's composite; a third layer
        // (237, model11) links the middle layer's composite in turn. The worry
        // this pins: does the source's own (unresolved, no-camera) self-link
        // leave `id_link_map`/the shared-copy bookkeeping in a state that
        // breaks the two REAL cross-layer hops downstream of it.
        addNode(424, "source_with_self_link", { GenLinkTex(424) });
        addNode(420, "middle_reader", { GenLinkTex(424) });
        addNode(237, "final_reader", { GenLinkTex(420) });

        auto rgraph = wallpaper::sceneToRenderGraph(scene);
        CHECK_FALSE(rgraph->HasCycle());

        // Each cross-layer hop must land on a real, non-empty texture key --
        // not the empty string a failed/unresolved link falls back to.
        CHECK(ResolvedTexAt(*rgraph, "middle_reader", 0) == GenLinkTex(424));
        CHECK(ResolvedTexAt(*rgraph, "final_reader", 0) == GenLinkTex(420));
    }
}
