#include <doctest.h>

#include "VulkanRender/SceneToRenderGraph.hpp"
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
}
