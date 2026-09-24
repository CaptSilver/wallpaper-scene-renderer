#include <doctest.h>

#include "UserPropEffectVisResolve.hpp"

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

TEST_SUITE("UserPropEffectVisResolve") {
    TEST_CASE("a compile-failure compaction shifts the bound effect's recorded index to match") {
        auto                  worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), "_rt_a", "_rt_b");
        layer.AddEffect(makeEffectWithNode("pulse", makeNodeWithCodes({ goodCode() })));
        auto broken =
            makeEffectWithNode("broken", makeNodeWithCodes({})); // empty codes = failed compile
        layer.AddEffect(broken);
        auto bound = makeEffectWithNode("bound", makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(bound);
        // A fourth, always-visible effect keeps "bound" from landing last
        // after compaction -- this test is about index correctness, not the
        // separate last-effect exclusion (covered by the next case).
        layer.AddEffect(makeEffectWithNode("trailer", makeNodeWithCodes({ goodCode() })));
        REQUIRE(layer.EffectCount() == 4);
        REQUIRE(layer.RemoveFailedEffects() == 1); // drops "broken" at index 1
        REQUIRE(layer.EffectCount() == 3);
        REQUIRE(layer.GetEffect(1).get() == bound.get()); // "bound" is now at index 1, was 2

        std::unordered_map<i32, SceneImageEffectLayer*> nodeMap { { 42, &layer } };
        std::vector<wek::PendingUserPropEffectVis>      pending {
            { bound.get(), 42, true, "{}", "godrays_enable" }
        };
        std::unordered_map<std::string, std::vector<Scene::UserPropEffectVisibility>> bindings;

        wek::resolvePendingUserPropEffectVis(pending, nodeMap, bindings);

        REQUIRE(bindings.count("godrays_enable") == 1);
        REQUIRE(bindings.at("godrays_enable").size() == 1);
        CHECK(bindings.at("godrays_enable")[0].effectIndex ==
              1); // NOT 2 -- the pre-compaction position
        CHECK(bindings.at("godrays_enable")[0].nodeId == 42);
    }

    TEST_CASE("an effect that ends up last on its layer is left unregistered") {
        auto                  worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), "_rt_a", "_rt_b");
        layer.AddEffect(makeEffectWithNode("pulse", makeNodeWithCodes({ goodCode() })));
        auto bound = makeEffectWithNode("bound", makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(bound); // "bound" is index 1 -- the last one on this layer

        std::unordered_map<i32, SceneImageEffectLayer*> nodeMap { { 42, &layer } };
        std::vector<wek::PendingUserPropEffectVis>      pending {
            { bound.get(), 42, true, "{}", "godrays_enable" }
        };
        std::unordered_map<std::string, std::vector<Scene::UserPropEffectVisibility>> bindings;

        wek::resolvePendingUserPropEffectVis(pending, nodeMap, bindings);

        CHECK(bindings.count("godrays_enable") == 0);
    }

    TEST_CASE("an effect RemoveFailedEffects dropped entirely is left unregistered") {
        auto                  worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), "_rt_a", "_rt_b");
        layer.AddEffect(makeEffectWithNode("pulse", makeNodeWithCodes({ goodCode() })));
        auto bound =
            makeEffectWithNode("bound", makeNodeWithCodes({})); // empty codes = failed compile
        layer.AddEffect(bound);
        layer.AddEffect(makeEffectWithNode("trailer", makeNodeWithCodes({ goodCode() })));
        REQUIRE(layer.RemoveFailedEffects() == 1); // drops "bound"

        std::unordered_map<i32, SceneImageEffectLayer*> nodeMap { { 42, &layer } };
        std::vector<wek::PendingUserPropEffectVis>      pending {
            { bound.get(), 42, true, "{}", "godrays_enable" }
        };
        std::unordered_map<std::string, std::vector<Scene::UserPropEffectVisibility>> bindings;

        wek::resolvePendingUserPropEffectVis(pending, nodeMap, bindings);

        CHECK(bindings.count("godrays_enable") == 0);
    }
}
