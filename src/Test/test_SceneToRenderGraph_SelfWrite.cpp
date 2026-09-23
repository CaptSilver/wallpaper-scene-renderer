#include <doctest.h>

#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/CopyPass.hpp"
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

TEST_SUITE("SceneToRenderGraph_SelfWrite") {
    TEST_CASE("two nodes hazarding the same render target each get their own copy pass") {
        Scene scene;
        auto  addHazardNode = [&](i32 id, std::string_view passName) {
            auto          node = std::make_shared<SceneNode>();
            auto          mesh = std::make_shared<SceneMesh>();
            SceneMaterial mat;
            mat.name     = std::string(passName);
            mat.textures = { std::string(SpecTex_Default) };
            mesh->AddMaterial(std::move(mat));
            node->AddMesh(mesh);
            node->ID() = id;
            scene.sceneGraph->AppendChild(node);
        };

        // Both nodes read AND write _rt_default -- the REFRACT-style self-write
        // hazard, one after the other in z-order.  Each hazard occurrence must
        // get its own copy of the target as it stood right before that node
        // drew; a design that shares one copy between them hands the second
        // node a snapshot from before the first node's own draw landed.
        addHazardNode(1001, "hazardNodeA");
        addHazardNode(1002, "hazardNodeB");

        auto rgraph = wallpaper::sceneToRenderGraph(scene);

        std::vector<vulkan::CopyPass*> copyPasses;
        for (auto id : rgraph->topologicalOrder()) {
            auto* pn = rgraph->getPassNode(id);
            if (pn == nullptr || pn->type() != rg::PassNode::Type::Copy) continue;
            copyPasses.push_back(dynamic_cast<vulkan::CopyPass*>(rgraph->getPass(id)));
        }

        REQUIRE(copyPasses.size() == 2u);
        REQUIRE(copyPasses[0] != nullptr);
        REQUIRE(copyPasses[1] != nullptr);
        // Distinct destination keys prove these are two independently-versioned
        // snapshots, not the same copy pass walked twice.
        CHECK(copyPasses[0]->desc().dst != copyPasses[1]->desc().dst);
    }
}
