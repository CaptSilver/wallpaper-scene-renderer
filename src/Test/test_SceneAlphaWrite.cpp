#include <doctest.h>

#include "SceneAlphaWrite.hpp"

#include "Scene/Scene.h"
#include "Scene/SceneImageEffectLayer.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneNode.h"

using namespace wallpaper;

namespace
{

// Mirrors what the parser seeds into every image/solidlayer material: all four
// alpha-carrying names are present in constValues regardless of which one the
// bound shader declares, so the name alone can't tell them apart.
std::shared_ptr<SceneNode> makeLayerNode(float alpha, float flatAlpha) {
    auto          node = std::make_shared<SceneNode>();
    auto          mesh = std::make_shared<SceneMesh>();
    SceneMaterial mat;
    mat.customShader.constValues["g_Color4"]    = std::array<float, 4> { 1.f, 1.f, 1.f, alpha };
    mat.customShader.constValues["g_UserAlpha"] = alpha;
    mat.customShader.constValues["g_Color"]     = std::array<float, 3> { 1.f, 1.f, 1.f };
    mat.customShader.constValues["g_Alpha"]     = flatAlpha;
    mesh->AddMaterial(std::move(mat));
    node->AddMesh(mesh);
    return node;
}

float uniform(SceneNode& node, const char* name, std::size_t index = 0) {
    auto& cv = node.Mesh()->Material()->customShader.constValues;
    REQUIRE(cv.count(name) == 1u);
    return cv.at(name)[index];
}

} // namespace

TEST_SUITE("SceneAlphaWrite") {
    TEST_CASE("alpha reaches every uniform a WE shader may read it from") {
        auto  node = makeLayerNode(1.0f, 1.0f);
        Scene scene;

        wek::writeNodeAlpha(scene, node.get(), 7, 0.25f);

        CHECK(uniform(*node, "g_UserAlpha") == doctest::Approx(0.25f));
        // genericimage4 and the text path have no g_UserAlpha at all.
        CHECK(uniform(*node, "g_Color4", 3) == doctest::Approx(0.25f));
        // flat.frag (solidlayer) reads neither of the two above.
        CHECK(uniform(*node, "g_Alpha") == doctest::Approx(0.25f));
        CHECK(node->Mesh()->Material()->customShader.constValuesDirty);
    }

    TEST_CASE("effect chain: every stage takes the alpha, the pingpong input keeps its own") {
        auto  base = makeLayerNode(1.0f, 0.0f); // solidlayer placeholder: g_Alpha starts at 0
        Scene scene;

        SceneImageEffectLayer layer(base.get(), "_rt_pingpong_a", "_rt_pingpong_b");
        auto                  effectNode = makeLayerNode(1.0f, 1.0f);
        auto                  effect     = std::make_shared<SceneImageEffect>();
        effect->name                     = "opacity";
        effect->nodes.push_back({ "_rt_pingpong_b", effectNode });
        layer.AddEffect(effect);

        SceneMaterial finalMat;
        finalMat.customShader.constValues["g_Color4"] = std::array<float, 4> { 1.f, 1.f, 1.f, 1.f };
        finalMat.customShader.constValues["g_UserAlpha"] = 1.0f;
        finalMat.customShader.constValues["g_Alpha"]     = 1.0f;
        layer.FinalMesh().AddMaterial(std::move(finalMat));

        scene.nodeEffectLayerMap[7] = &layer;

        wek::writeNodeAlpha(scene, base.get(), 7, 0.5f);

        CHECK(uniform(*effectNode, "g_UserAlpha") == doctest::Approx(0.5f));
        CHECK(uniform(*effectNode, "g_Color4", 3) == doctest::Approx(0.5f));
        CHECK(uniform(*effectNode, "g_Alpha") == doctest::Approx(0.5f));

        auto& finalCv = layer.FinalMesh().Material()->customShader.constValues;
        CHECK(finalCv.at("g_UserAlpha")[0] == doctest::Approx(0.5f));
        CHECK(finalCv.at("g_Alpha")[0] == doctest::Approx(0.5f));

        // The base pass feeds the chain's first pingpong.  A solidlayer's flat
        // quad is deliberately transparent there so the chain starts from a
        // clean (0,0,0,0); raising it would paint a coloured rectangle behind
        // the effect output.
        CHECK(uniform(*base, "g_Alpha") == doctest::Approx(0.0f));
        CHECK(uniform(*base, "g_UserAlpha") == doctest::Approx(0.5f));
    }
}
