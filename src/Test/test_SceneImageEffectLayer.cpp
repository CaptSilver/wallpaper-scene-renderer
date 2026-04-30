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

// ─────────────────────────────────────────────────────────────────────────────
// SceneImageEffectLayer::ResolveEffect
//
// Wires the per-effect ping-pong RTs and finalises the last effect node's
// output target, blend mode, camera, transform, and parent chain based on
// whether the layer is offscreen / passthrough / has a parent group / has
// a parent proxy.  See translucent-blend-bug.md and we-architecture-rewrite.md.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

// Build a generic default mesh whose shared_ptr<Data> can be aliased into
// the effect node's mesh by ChangeMeshDataFrom.  Any non-null SceneMesh
// works — ResolveEffect only swaps the data pointer, never reads vertices.
SceneMesh makeDefaultMesh() { return SceneMesh(false); }

} // namespace

TEST_SUITE("SceneImageEffectLayer::ResolveEffect")
{
    // ── ping-pong RT substitution ───────────────────────────────────────────

    TEST_CASE("PPONG_A prefix on cmd.src is replaced with concrete pingpong_a") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA",
                                    "PPB");
        auto eff = makeEffectWithNode("e1",
                                      makeNodeWithCodes({ goodCode() }));
        SceneImageEffect::Command cmd;
        cmd.cmd = SceneImageEffect::CmdType::Copy;
        cmd.src = "_rt_effect_pingpong_a_PLACEHOLDER";
        cmd.dst = "_rt_some_other_target";
        eff->commands.push_back(cmd);
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effectCam");

        CHECK(layer.GetEffect(0)->commands[0].src == "PPA");
        // dst doesn't have the PPONG_A prefix → stays as-is.
        CHECK(layer.GetEffect(0)->commands[0].dst == "_rt_some_other_target");
    }

    TEST_CASE("PPONG_A prefix on cmd.dst is replaced with concrete pingpong_a") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA-DST",
                                    "PPB");
        auto eff = makeEffectWithNode("e1",
                                      makeNodeWithCodes({ goodCode() }));
        SceneImageEffect::Command cmd;
        cmd.dst = "_rt_effect_pingpong_a_PLACEHOLDER";
        cmd.src = "verbatim";
        eff->commands.push_back(cmd);
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effectCam");

        CHECK(layer.GetEffect(0)->commands[0].dst == "PPA-DST");
        CHECK(layer.GetEffect(0)->commands[0].src == "verbatim");
    }

    TEST_CASE("commands without PPONG_A prefix pass through unchanged") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "_rt_effect_pingpong_a_X",
                                    "_rt_effect_pingpong_b_X");
        auto eff = makeEffectWithNode("e1",
                                      makeNodeWithCodes({ goodCode() }));
        SceneImageEffect::Command cmd;
        cmd.src = "_rt_external_buffer";
        cmd.dst = "_rt_someother";
        eff->commands.push_back(cmd);
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effectCam");

        CHECK(layer.GetEffect(0)->commands[0].src == "_rt_external_buffer");
        CHECK(layer.GetEffect(0)->commands[0].dst == "_rt_someother");
    }

    // ── per-node output substitution ────────────────────────────────────────

    TEST_CASE("node output starting with PPONG_B prefix → replaced with pingpong_b") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA",
                                    "PPB-RESOLVED");
        // Default makeEffectWithNode output is "_rt_effect_pingpong_b_"
        layer.AddEffect(makeEffectWithNode("e1",
                                           makeNodeWithCodes({ goodCode() })));
        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effectCam");

        // The intermediate node output gets routed to ppong_b...  but since
        // this node was identified as last_output, the final block overwrites
        // it with SpecTex_Default (m_is_offscreen=false default).
        auto& node = layer.GetEffect(0)->nodes.front();
        CHECK(node.output == "_rt_default");
    }

    TEST_CASE("node output equal to SpecTex_Default → also picked as last_output") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        auto eff = makeEffectWithNode("e1",
                                      makeNodeWithCodes({ goodCode() }),
                                      "_rt_default");
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effectCam");

        CHECK(layer.GetEffect(0)->nodes.front().output == "_rt_default");
        CHECK(layer.ResolvedLastOutput() != nullptr);
    }

    TEST_CASE("node output not matching either → not picked as last_output, stays unchanged") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        // Output is unrelated to either prefix or default.
        auto eff = makeEffectWithNode("e1",
                                      makeNodeWithCodes({ goodCode() }),
                                      "_rt_custom_target");
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effectCam");

        // ResolveEffect skipped the output-substitution branch, last_output
        // stayed null, so the final block was never entered.  Output stays.
        CHECK(layer.GetEffect(0)->nodes.front().output == "_rt_custom_target");
        CHECK(layer.ResolvedLastOutput() == nullptr);
    }

    // ── texture replace_if for PPONG_PREFIX_A ───────────────────────────────

    TEST_CASE("textures with PPONG_A prefix get rewritten to ppong_a") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "RESOLVED-A",
                                    "RESOLVED-B");
        auto node = makeNodeWithCodes({ goodCode() });
        auto* mat = node->Mesh()->Material();
        mat->textures = { "_rt_effect_pingpong_a_x",
                          "tex2.png",
                          "_rt_effect_pingpong_a_other" };
        auto eff = makeEffectWithNode("e1", node);
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effectCam");

        const auto& tex = layer.GetEffect(0)
                              ->nodes.front()
                              .sceneNode->Mesh()
                              ->Material()
                              ->textures;
        CHECK(tex[0] == "RESOLVED-A");
        CHECK(tex[1] == "tex2.png");          // unchanged
        CHECK(tex[2] == "RESOLVED-A");
    }

    // ── swap_pp between effects ─────────────────────────────────────────────

    TEST_CASE("swap_pp alternates ppong_a/ppong_b between consecutive effects") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "ABCDEF",
                                    "FEDCBA");

        // Effect 1 — uses "current" ppong_a as cmd src.
        auto e1 = makeEffectWithNode("e1",
                                     makeNodeWithCodes({ goodCode() }));
        SceneImageEffect::Command c1;
        c1.src = "_rt_effect_pingpong_a_x";
        e1->commands.push_back(c1);
        layer.AddEffect(e1);

        // Effect 2 — after swap, ppong_a should now equal "FEDCBA".
        auto e2 = makeEffectWithNode("e2",
                                     makeNodeWithCodes({ goodCode() }));
        SceneImageEffect::Command c2;
        c2.src = "_rt_effect_pingpong_a_x";
        e2->commands.push_back(c2);
        layer.AddEffect(e2);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effectCam");

        CHECK(layer.GetEffect(0)->commands[0].src == "ABCDEF");
        CHECK(layer.GetEffect(1)->commands[0].src == "FEDCBA");
    }

    TEST_CASE("swap_pp with three effects: ABA pattern") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "AAA",
                                    "BBB");
        for (int i = 0; i < 3; i++) {
            auto e = makeEffectWithNode("eN",
                                         makeNodeWithCodes({ goodCode() }));
            SceneImageEffect::Command cmd;
            cmd.src = "_rt_effect_pingpong_a_x";
            e->commands.push_back(cmd);
            layer.AddEffect(e);
        }
        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effectCam");

        CHECK(layer.GetEffect(0)->commands[0].src == "AAA");
        CHECK(layer.GetEffect(1)->commands[0].src == "BBB");
        CHECK(layer.GetEffect(2)->commands[0].src == "AAA");
    }

    // ── basic per-node mutation (blend, camera, transform) ──────────────────

    TEST_CASE("each effect node gets blend=Normal and the supplied effect_cam") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        auto node = makeNodeWithCodes({ goodCode() });
        auto* mat = node->Mesh()->Material();
        mat->blenmode = BlendMode::Additive;          // start with non-default
        node->SetTranslate({ 7.f, 8.f, 9.f });        // start with non-zero
        auto eff = makeEffectWithNode("e1", node, "intermediate-only");
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "myEffectCam");

        // Loop body sets blend=Normal even on intermediate nodes.
        CHECK(node->Mesh()->Material()->blenmode == BlendMode::Normal);
        // Camera was overwritten.
        CHECK(node->Camera() == "myEffectCam");
        // Transform reset to default (zero translate).
        CHECK(node->Translate().x() == doctest::Approx(0.0f));
        CHECK(node->Translate().y() == doctest::Approx(0.0f));
        CHECK(node->Translate().z() == doctest::Approx(0.0f));
    }

    // ── last_output: m_is_offscreen path ────────────────────────────────────

    TEST_CASE("offscreen layer: last_output writes to per-node offscreen RT, blend=Normal") {
        auto worldNode = std::make_shared<SceneNode>();
        worldNode->ID() = 42;
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        layer.SetOffscreen(true);
        layer.SetFinalBlend(BlendMode::Additive); // should be ignored when offscreen

        auto eff = makeEffectWithNode("e1",
                                       makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "offCam");

        auto& node = layer.GetEffect(0)->nodes.front();
        CHECK(node.output == "_rt_offscreen_42");
        // Final blend is forced to Normal in offscreen mode (Vulkan UB
        // workaround for DONT_CARE load op on uninitialised RTs).
        CHECK(node.sceneNode->Mesh()->Material()->blenmode == BlendMode::Normal);
        // Camera = effect_cam (NOT m_final_camera — offscreen ignores it).
        CHECK(node.sceneNode->Camera() == "offCam");
    }

    TEST_CASE("offscreen layer: ID=0 still produces a valid offscreen RT name") {
        auto worldNode = std::make_shared<SceneNode>();
        worldNode->ID() = 0;
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        layer.SetOffscreen(true);
        auto eff = makeEffectWithNode("e1",
                                       makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "offCam");

        CHECK(layer.GetEffect(0)->nodes.front().output == "_rt_offscreen_0");
    }

    // ── last_output: m_passthrough path ─────────────────────────────────────

    TEST_CASE("passthrough layer: last_output uses final_camera, final_blend, final_node trans") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        layer.SetPassthrough(true);
        layer.SetFinalBlend(BlendMode::Translucent);
        layer.SetFinalCamera("composeCam");
        // Stage final_node with a non-zero translate; ResolveEffect should
        // copy that into last_output's transform.
        layer.FinalNode().SetTranslate({ 11.f, 22.f, 33.f });

        auto eff = makeEffectWithNode("e1",
                                       makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effCam");

        auto& node = layer.GetEffect(0)->nodes.front();
        CHECK(node.output == "_rt_default");
        CHECK(node.sceneNode->Mesh()->Material()->blenmode == BlendMode::Translucent);
        CHECK(node.sceneNode->Camera() == "composeCam");
        CHECK(node.sceneNode->Translate().x() == doctest::Approx(11.0f));
        CHECK(node.sceneNode->Translate().y() == doctest::Approx(22.0f));
        CHECK(node.sceneNode->Translate().z() == doctest::Approx(33.0f));
    }

    // ── last_output: normal (non-offscreen, non-passthrough) path ───────────

    TEST_CASE("normal layer: last_output gets _rt_default, final_blend, final_camera, final_trans") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        layer.SetFinalBlend(BlendMode::Translucent_PA);
        layer.SetFinalCamera("orthoCam");
        layer.FinalNode().SetTranslate({ 1.f, 2.f, 3.f });

        auto eff = makeEffectWithNode("e1",
                                       makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effCam");

        auto& node = layer.GetEffect(0)->nodes.front();
        CHECK(node.output == "_rt_default");
        CHECK(node.sceneNode->Mesh()->Material()->blenmode == BlendMode::Translucent_PA);
        CHECK(node.sceneNode->Camera() == "orthoCam");
        CHECK(node.sceneNode->Translate().x() == doctest::Approx(1.0f));
        CHECK(node.sceneNode->Translate().y() == doctest::Approx(2.0f));
        CHECK(node.sceneNode->Translate().z() == doctest::Approx(3.0f));
    }

    // ── m_inherit_parent + m_parent_proxy ───────────────────────────────────

    TEST_CASE("inherit_parent without proxy → InheritParent from world node") {
        auto worldNode = std::make_shared<SceneNode>();
        // worldNode has its own parent that we expect to be inherited.
        auto worldParent = std::make_shared<SceneNode>();
        worldParent->AppendChild(worldNode);

        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        layer.SetInheritParent(true);

        auto eff = makeEffectWithNode("e1",
                                       makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effCam");

        // last_output's parent should match worldNode's parent.
        auto& node = layer.GetEffect(0)->nodes.front();
        CHECK(node.sceneNode->Parent() == worldParent.get());
    }

    TEST_CASE("inherit_parent WITH proxy → SetParent to the proxy directly") {
        auto worldNode    = std::make_shared<SceneNode>();
        auto proxyParent  = std::make_shared<SceneNode>();
        proxyParent->SetTranslate({ 50.f, 0.f, 0.f });

        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        layer.SetInheritParent(true);
        layer.SetParentProxy(proxyParent);

        auto eff = makeEffectWithNode("e1",
                                       makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effCam");

        auto& node = layer.GetEffect(0)->nodes.front();
        CHECK(node.sceneNode->Parent() == proxyParent.get());
    }

    TEST_CASE("inherit_parent=false → no parent wiring (parent stays nullptr)") {
        auto worldNode   = std::make_shared<SceneNode>();
        auto worldParent = std::make_shared<SceneNode>();
        worldParent->AppendChild(worldNode);

        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        // Don't call SetInheritParent — stays false.

        auto eff = makeEffectWithNode("e1",
                                       makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effCam");

        auto& node = layer.GetEffect(0)->nodes.front();
        CHECK(node.sceneNode->Parent() == nullptr);
    }

    TEST_CASE("inherit_parent + passthrough → still routes through proxy if present") {
        auto worldNode   = std::make_shared<SceneNode>();
        auto proxyParent = std::make_shared<SceneNode>();

        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        layer.SetPassthrough(true);
        layer.SetInheritParent(true);
        layer.SetParentProxy(proxyParent);

        auto eff = makeEffectWithNode("e1",
                                       makeNodeWithCodes({ goodCode() }));
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effCam");

        auto& node = layer.GetEffect(0)->nodes.front();
        CHECK(node.sceneNode->Parent() == proxyParent.get());
    }

    // ── ResolvedLastOutput accessor ─────────────────────────────────────────

    TEST_CASE("ResolvedLastOutput points to the final node's SceneNode") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        auto innerNode = makeNodeWithCodes({ goodCode() });
        auto eff       = makeEffectWithNode("e1", innerNode);
        layer.AddEffect(eff);

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effCam");

        CHECK(layer.ResolvedLastOutput() == innerNode.get());
    }

    TEST_CASE("ResolvedLastOutput is the LAST eligible node when chain has multiple") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        auto first  = makeNodeWithCodes({ goodCode() });
        auto second = makeNodeWithCodes({ goodCode() });
        layer.AddEffect(makeEffectWithNode("first", first));
        layer.AddEffect(makeEffectWithNode("second", second));

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effCam");

        // Final block runs once after the loop with last_output = second.
        CHECK(layer.ResolvedLastOutput() == second.get());
        // The first node's output should be "PPB" (intermediate ppong_b),
        // because the final-block override only touched the last node.
        CHECK(layer.GetEffect(0)->nodes.front().output == "PPB");
        // And the second node got rewritten to _rt_default by the final block.
        CHECK(layer.GetEffect(1)->nodes.front().output == "_rt_default");
    }

    // ── empty m_effects ─────────────────────────────────────────────────────

    TEST_CASE("empty effect list → no work, ResolvedLastOutput stays nullptr") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effCam");
        CHECK(layer.ResolvedLastOutput() == nullptr);
    }

    // ── all-non-eligible nodes (no last_output candidate) ───────────────────

    TEST_CASE("no node matches output substitution → final block is skipped entirely") {
        auto worldNode = std::make_shared<SceneNode>();
        SceneImageEffectLayer layer(worldNode.get(), 100, 100,
                                    "PPA", "PPB");
        // Outputs neither start with PPONG_B nor equal _rt_default.
        layer.AddEffect(makeEffectWithNode("e1",
                                            makeNodeWithCodes({ goodCode() }),
                                            "_rt_unmatched_a"));
        layer.AddEffect(makeEffectWithNode("e2",
                                            makeNodeWithCodes({ goodCode() }),
                                            "_rt_unmatched_b"));

        SceneMesh dm = makeDefaultMesh();
        layer.ResolveEffect(dm, "effCam");

        CHECK(layer.ResolvedLastOutput() == nullptr);
        CHECK(layer.GetEffect(0)->nodes.front().output == "_rt_unmatched_a");
        CHECK(layer.GetEffect(1)->nodes.front().output == "_rt_unmatched_b");
    }
}
