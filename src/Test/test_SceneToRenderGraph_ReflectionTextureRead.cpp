#include <doctest.h>

#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/CustomShaderPass.hpp"
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneCamera.h"
#include "SpecTexs.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace wallpaper;

namespace
{
std::shared_ptr<SceneNode> addNode(Scene& scene, i32 id, std::string name,
                                   std::vector<std::string> textures) {
    auto          node = std::make_shared<SceneNode>();
    auto          mesh = std::make_shared<SceneMesh>();
    SceneMaterial mat;
    mat.name     = std::move(name);
    mat.textures = std::move(textures);
    mesh->AddMaterial(std::move(mat));
    node->AddMesh(mesh);
    node->ID() = id;
    scene.sceneGraph->AppendChild(node);
    return node;
}

vulkan::CustomShaderPass* findReflPass(rg::RenderGraph& rgraph, std::string_view nameContains) {
    for (auto id : rgraph.topologicalOrder()) {
        auto* pn = rgraph.getPassNode(id);
        if (pn == nullptr || pn->type() != rg::PassNode::Type::CustomShader) continue;
        auto* p = dynamic_cast<vulkan::CustomShaderPass*>(rgraph.getPass(id));
        if (p && pn->name().find(nameContains) != std::string::npos) return p;
    }
    return nullptr;
}
} // namespace

TEST_SUITE("SceneToRenderGraph_ReflectionTextureRead") {
    TEST_CASE("a reflection-pass node's link-texture reference stays unresolved -- today's "
              "behavior, pinned so the dedup can't silently change it") {
        Scene scene;
        scene.cameras["reflected_perspective"] =
            std::make_shared<SceneCamera>(1.0f, 0.1f, 100.0f, 45.0f);
        // GenLinkTex(999) -- an ordinary cross-layer link reference, the same
        // shape test_SceneToRenderGraph_LinkResolution.cpp uses for ToGraphPass.
        addNode(scene, 42, "refl_reader", { GenLinkTex(999) });

        auto  rgraph = wallpaper::sceneToRenderGraph(scene);
        auto* pass   = findReflPass(*rgraph, "refl_");
        REQUIRE(pass != nullptr);
        REQUIRE(pass->desc().textures.size() == 1u);
        // Unresolved link tex binds to an empty slot -- same as an omitted
        // texture. If this ever starts resolving, it's a real behavior change
        // that needs its own task and its own render proof, not a side effect
        // of a dedup refactor.
        CHECK(pass->desc().textures[0].empty());
    }

    TEST_CASE("a reflection-pass node reading _rt_Reflection is skipped before the shared "
              "texture-read loop even runs") {
        Scene scene;
        scene.cameras["reflected_perspective"] =
            std::make_shared<SceneCamera>(1.0f, 0.1f, 100.0f, 45.0f);
        addNode(scene, 43, "grid_floor", { std::string(WE_REFLECTION) });

        auto rgraph = wallpaper::sceneToRenderGraph(scene);
        CHECK(findReflPass(*rgraph, "refl_grid_floor") == nullptr);
    }
}
