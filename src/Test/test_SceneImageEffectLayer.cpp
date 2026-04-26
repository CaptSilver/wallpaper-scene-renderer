#include <doctest.h>

#include "Scene/SceneImageEffectLayer.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneShader.h"

using namespace wallpaper;

namespace
{

// Build a SceneNode with a Material whose customShader has the given
// SPIR-V codes vector.  Empty codes simulates a failed compile.
std::shared_ptr<SceneNode> makeNodeWithCodes(std::vector<ShaderCode> codes) {
    auto node = std::make_shared<SceneNode>();
    auto mesh = std::make_shared<SceneMesh>();
    SceneMaterial mat;
    mat.customShader.shader        = std::make_shared<SceneShader>();
    mat.customShader.shader->codes = std::move(codes);
    mesh->AddMaterial(std::move(mat));
    node->AddMesh(mesh);
    return node;
}

std::shared_ptr<SceneImageEffect> makeEffectWithNode(std::string name,
                                                     std::shared_ptr<SceneNode> n,
                                                     std::string output = "_rt_effect_pingpong_b_") {
    auto eff  = std::make_shared<SceneImageEffect>();
    eff->name = std::move(name);
    eff->nodes.push_back({ output, std::move(n) });
    return eff;
}

ShaderCode goodCode() {
    return { 0x07230203 }; // SPIR-V magic — non-empty body, treated as compiled
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// SceneImageEffectLayer::RemoveFailedEffects
//
// Drops effects whose nodes have empty SPIR-V codes (compile failed during
// async flush).  Lets ResolveEffect promote the previous successfully-
// compiled effect as last_output instead of leaving a hole at the end of the
// chain.  See translucent-blend-bug.md and commits a0dc4fa / 09c3470.
// ─────────────────────────────────────────────────────────────────────────────

TEST_SUITE("SceneImageEffectLayer::RemoveFailedEffects")
{
    TEST_CASE("empty layer — nothing to remove") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "_rt_effect_pingpong_a_X",
                                    "_rt_effect_pingpong_b_X");
        CHECK(layer.RemoveFailedEffects() == 0);
        CHECK(layer.EffectCount() == 0);
    }

    TEST_CASE("all effects compiled — nothing removed") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "_rt_effect_pingpong_a_X",
                                    "_rt_effect_pingpong_b_X");
        layer.AddEffect(makeEffectWithNode("pulse",
                                            makeNodeWithCodes({ goodCode() })));
        layer.AddEffect(makeEffectWithNode("shake",
                                            makeNodeWithCodes({ goodCode() })));
        CHECK(layer.EffectCount() == 2);
        CHECK(layer.RemoveFailedEffects() == 0);
        CHECK(layer.EffectCount() == 2);
    }

    TEST_CASE("last effect compile failed — drops just that one") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "_rt_effect_pingpong_a_X",
                                    "_rt_effect_pingpong_b_X");
        layer.AddEffect(makeEffectWithNode("pulse",
                                            makeNodeWithCodes({ goodCode() })));
        layer.AddEffect(makeEffectWithNode("shake",
                                            makeNodeWithCodes({ goodCode() })));
        // Lens flare = workshop shader that fails HLSL→GLSL.  All its nodes
        // end up with empty codes after FlushPendingCompilations.
        layer.AddEffect(makeEffectWithNode("Lens Flare Sun",
                                            makeNodeWithCodes({})));
        CHECK(layer.EffectCount() == 3);
        CHECK(layer.RemoveFailedEffects() == 1);
        CHECK(layer.EffectCount() == 2);
        // Order preserved — the survivors are the first two.
        CHECK(layer.GetEffect(0)->name == "pulse");
        CHECK(layer.GetEffect(1)->name == "shake");
    }

    TEST_CASE("middle effect failed — chain stays linear, second pulse drops") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "_rt_effect_pingpong_a_X",
                                    "_rt_effect_pingpong_b_X");
        layer.AddEffect(makeEffectWithNode("pulse",
                                            makeNodeWithCodes({ goodCode() })));
        layer.AddEffect(makeEffectWithNode("broken_eff",
                                            makeNodeWithCodes({})));
        layer.AddEffect(makeEffectWithNode("shake",
                                            makeNodeWithCodes({ goodCode() })));
        CHECK(layer.RemoveFailedEffects() == 1);
        CHECK(layer.EffectCount() == 2);
        CHECK(layer.GetEffect(0)->name == "pulse");
        CHECK(layer.GetEffect(1)->name == "shake");
    }

    TEST_CASE("multi-node effect — kept if any node compiled") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "_rt_effect_pingpong_a_X",
                                    "_rt_effect_pingpong_b_X");
        auto eff = std::make_shared<SceneImageEffect>();
        eff->name = "multi";
        eff->nodes.push_back({ "_rt_effect_pingpong_b_", makeNodeWithCodes({}) });
        eff->nodes.push_back({ "_rt_effect_pingpong_b_", makeNodeWithCodes({ goodCode() }) });
        layer.AddEffect(eff);
        CHECK(layer.RemoveFailedEffects() == 0);
        CHECK(layer.EffectCount() == 1);
    }

    TEST_CASE("multi-node effect — all nodes failed → drops") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "_rt_effect_pingpong_a_X",
                                    "_rt_effect_pingpong_b_X");
        auto eff = std::make_shared<SceneImageEffect>();
        eff->name = "all_failed";
        eff->nodes.push_back({ "_rt_effect_pingpong_b_", makeNodeWithCodes({}) });
        eff->nodes.push_back({ "_rt_effect_pingpong_b_", makeNodeWithCodes({}) });
        layer.AddEffect(eff);
        CHECK(layer.RemoveFailedEffects() == 1);
        CHECK(layer.EffectCount() == 0);
    }

    TEST_CASE("effect with no nodes — kept (defensive: empty list isn't a "
              "compile failure marker)") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "_rt_effect_pingpong_a_X",
                                    "_rt_effect_pingpong_b_X");
        auto eff  = std::make_shared<SceneImageEffect>();
        eff->name = "empty_nodes";
        layer.AddEffect(eff);
        CHECK(layer.RemoveFailedEffects() == 0);
        CHECK(layer.EffectCount() == 1);
    }

    TEST_CASE("node without material — treated as not-compiled and drops") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "_rt_effect_pingpong_a_X",
                                    "_rt_effect_pingpong_b_X");
        auto eff = std::make_shared<SceneImageEffect>();
        eff->name = "no_material";
        // Plain SceneNode with no mesh/material attached.
        eff->nodes.push_back({ "_rt_effect_pingpong_b_",
                               std::make_shared<SceneNode>() });
        layer.AddEffect(eff);
        CHECK(layer.RemoveFailedEffects() == 1);
        CHECK(layer.EffectCount() == 0);
    }
}
