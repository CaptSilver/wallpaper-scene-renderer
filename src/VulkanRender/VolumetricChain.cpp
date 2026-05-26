#include "VulkanRender/VolumetricChain.hpp"

#include "Scene/SceneNode.h"
#include "Scene/SceneLight.hpp"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"
#include "SpecTexs.hpp"
#include "VulkanRender/CustomShaderPass.hpp"
#include "WPShaderValueUpdater.hpp"
#include "wpscene/WPMaterial.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace wallpaper::vulkan
{

namespace
{

// Half-res HDR scratch buffer for the volumetric chain.  RGBA16F regardless
// of Scene::hdrContent: per-light scatter is additive across N lights, so
// even an SDR-output scene needs floating-point headroom in the accumulator
// to avoid banding/clipping before the final tonemap on combine.
void registerHalfResRT(Scene& scene, std::string_view key) {
    auto& rt      = scene.renderTargets[std::string(key)];
    rt.width      = 2;
    rt.height     = 2;
    rt.allowReuse = true;
    rt.format     = TextureFormat::RGBA16F;
    rt.bind       = { .enable = true, .screen = true, .scale = 0.5 };
}

// Wraps a fresh SceneMesh around src's vertex/index data (SceneMesh holds the
// data via shared_ptr — ChangeMeshDataFrom is a shallow ptr assign, not a deep
// copy).  The returned mesh carries an independent material + dirty flag, which
// is the actually-desired behavior; only the geometry is shared.
std::shared_ptr<SceneMesh> aliasMeshData(const SceneMesh& src) {
    auto m = std::make_shared<SceneMesh>();
    m->ChangeMeshDataFrom(src);
    return m;
}

// Shared body for the two blur passes (h, v).  Each is a fullscreen quad that
// samples `in_key` and writes `out_key`.  Pulled out because the two callers
// differ only in name + IO RT keys + which global node to feed.
void emitVolumetricBlurOnePass(rg::RenderGraph& rgraph, std::string_view name,
                               std::string_view in_key, std::string_view out_key,
                               SceneNode* node) {
    rgraph.addPass<CustomShaderPass>(
        name,
        rg::PassNode::Type::CustomShader,
        [node, in_key, out_key](rg::RenderGraphBuilder&  builder,
                                CustomShaderPass::Desc& pdesc) {
            pdesc.node     = node;
            pdesc.output   = std::string(out_key);
            pdesc.textures = { std::string(in_key) };

            auto* in = builder.createTexNode(
                rg::TexNode::Desc { .name = std::string(in_key),
                                    .key  = std::string(in_key),
                                    .type = rg::TexNode::TexType::Temp });
            builder.read(in);

            auto* out = builder.createTexNode(
                rg::TexNode::Desc { .name = std::string(out_key),
                                    .key  = std::string(out_key),
                                    .type = rg::TexNode::TexType::Temp },
                /*write=*/true);
            builder.write(out);
        });
}

} // namespace

uint32_t volumetric_quality_from_pp_tier(std::string_view pp) {
    // WE volumetricsfront.frag branches on QUALITY for ray-march step count.
    // displayhdr matches ultra here — both pick the highest-quality 4-step
    // march.  Unknown / empty defaults to the medium tier so a misconfigured
    // wallpaper still renders volumetrics instead of failing closed.
    if (pp == "ultra" || pp == "displayhdr") return 4u;
    if (pp == "low") return 1u;
    return 2u;
}

wpscene::WPMaterial buildVolumetricBackMaterial(uint32_t quality) {
    wpscene::WPMaterial m;
    m.shader              = "volumetricsback";
    m.cullmode            = "front";
    m.blending            = "normal";
    m.depthtest           = "enabled";
    m.depthwrite          = "disabled";
    m.combos["QUALITY"]   = static_cast<int32_t>(quality);
    return m;
}

wpscene::WPMaterial buildVolumetricFrontMaterial(uint32_t quality) {
    wpscene::WPMaterial m;
    m.shader                 = "volumetricsfront";
    m.cullmode               = "back";
    m.blending               = "additive";
    m.depthtest              = "disabled";
    m.depthwrite             = "disabled";
    m.textures               = { std::string(WE_VOLUMETRICS_BACK) };
    m.combos["QUALITY"]      = static_cast<int32_t>(quality);
    m.combos["POINTLIGHT"]   = 1;
    m.combos["FULLSCREEN"]   = 0;
    m.combos["SHADOW"]       = 0;
    m.combos["REVERSEDEPTH"] = 0;
    return m;
}

wpscene::WPMaterial buildVolumetricFullscreenMaterial(uint32_t quality) {
    wpscene::WPMaterial m   = buildVolumetricFrontMaterial(quality);
    m.cullmode              = "nocull";
    m.combos["FULLSCREEN"]  = 1;
    return m;
}

wpscene::WPMaterial buildVolumetricBlurMaterial(int vertical) {
    wpscene::WPMaterial m;
    m.shader   = "blur_k3";
    m.cullmode = "nocull";
    m.blending = "normal";
    m.depthtest  = "disabled";
    m.depthwrite = "disabled";
    // Horizontal pass samples the per-light accumulator; vertical pass samples
    // the ping-pong scratch buffer.  Matches the I/O wiring in
    // emitVolumetricBlurPasses.
    m.textures = { std::string(vertical == 0 ? WE_VOLUMETRICS_LIGHT_BUFFER
                                             : WE_VOLUMETRICS_LIGHT_BUFFER_B) };
    m.combos["VERTICAL"] = vertical;
    return m;
}

wpscene::WPMaterial buildVolumetricCombineMaterial() {
    wpscene::WPMaterial m;
    m.shader     = "passthrough";
    m.cullmode   = "nocull";
    m.blending   = "additive";
    m.depthtest  = "disabled";
    m.depthwrite = "disabled";
    m.textures   = { std::string(WE_VOLUMETRICS_LIGHT_BUFFER) };
    return m;
}

void BuildVolumetricNodes(Scene& scene) {
    if (scene.volumetricsConfig.per_light.empty()) return;

    // Resolve the QUALITY combo value from the scene's post-processing tier
    // before constructing nodes so any consumer that consults
    // volumetricsConfig.quality between BuildVolumetricNodes and the material
    // attach step sees the resolved value (today: WPSceneParser's attach loop;
    // tomorrow: anything that wants to mirror the QUALITY decision).
    scene.volumetricsConfig.quality =
        volumetric_quality_from_pp_tier(scene.resolved_postprocessing);

    for (auto& pl : scene.volumetricsConfig.per_light) {
        if (pl.light == nullptr) continue;
        auto* lnode = pl.light->node();
        if (lnode == nullptr) continue;
        lnode->UpdateTrans();
        const auto&           world = lnode->ModelTrans();
        const Eigen::Vector3f origin(static_cast<float>(world(0, 3)),
                                     static_cast<float>(world(1, 3)),
                                     static_cast<float>(world(2, 3)));
        const float           radius = pl.light->radius();
        const Eigen::Vector3f scale(radius, radius, radius);
        const Eigen::Vector3f angles(0.0f, 0.0f, 0.0f);

        auto back = std::make_shared<SceneNode>(origin, scale, angles);
        back->AddMesh(aliasMeshData(scene.default_volume_sphere));

        auto front = std::make_shared<SceneNode>(origin, scale, angles);
        front->AddMesh(aliasMeshData(scene.default_volume_sphere));

        auto fullscreen = std::make_shared<SceneNode>();
        fullscreen->AddMesh(aliasMeshData(scene.default_effect_mesh));

        pl.back_node       = back;
        pl.front_node      = front;
        pl.fullscreen_node = fullscreen;
    }

    auto& cfg = scene.volumetricsConfig;
    if (! cfg.blur_h_node) {
        cfg.blur_h_node = std::make_shared<SceneNode>();
        cfg.blur_h_node->AddMesh(aliasMeshData(scene.default_effect_mesh));
    }
    if (! cfg.blur_v_node) {
        cfg.blur_v_node = std::make_shared<SceneNode>();
        cfg.blur_v_node->AddMesh(aliasMeshData(scene.default_effect_mesh));
    }
    if (! cfg.combine_node) {
        cfg.combine_node = std::make_shared<SceneNode>();
        cfg.combine_node->AddMesh(aliasMeshData(scene.default_effect_mesh));
    }

    registerHalfResRT(scene, WE_VOLUMETRICS_BACK);
    registerHalfResRT(scene, WE_VOLUMETRICS_LIGHT_BUFFER);
    registerHalfResRT(scene, WE_VOLUMETRICS_LIGHT_BUFFER_B);
}

// ---------------------------------------------------------------------------
// Volumetric chain render-graph emitters.
//
// All helpers share the same lambda shape used by SceneToRenderGraph.cpp:
// addPass<CustomShaderPass>(name, type, [...captures...](builder, pdesc)).
// Capturing-by-reference is safe — the lambda runs synchronously inside
// addPass (before it returns), so the Scene / PerLight references it grabs
// remain live for the call.
// ---------------------------------------------------------------------------

void emitVolumetricBackPass(rg::RenderGraph& rgraph, Scene& scene, size_t light_idx,
                            ExtraInfoVolumetrics& /*extra*/) {
    auto& pl = scene.volumetricsConfig.per_light.at(light_idx);
    if (! pl.back_node) return;

    std::string passName = "volumetrics_back_" + std::to_string(light_idx);
    rgraph.addPass<CustomShaderPass>(
        passName,
        rg::PassNode::Type::CustomShader,
        [&pl](rg::RenderGraphBuilder& builder, CustomShaderPass::Desc& pdesc) {
            pdesc.node               = pl.back_node.get();
            pdesc.output             = std::string(WE_VOLUMETRICS_BACK);
            // Per-light back-depth requires CLEAR every pass: multiple lights
            // share the same back RT, and the one-shot clearedRTs gate would
            // leak earlier lights' depth values into later lights' integration.
            pdesc.force_clear_output = true;
            // Sample the main scene depth to clip the back-face write against
            // foreground occluders — opaque scene geometry in front of the
            // light volume should kill the back-depth contribution.
            pdesc.needsSceneDepth = true;
            // Rasterise the proxy sphere's BACK faces — this pass captures
            // the volume's exit depth.
            pdesc.flipCullMode = true;

            auto* out = builder.createTexNode(
                rg::TexNode::Desc { .name = std::string(WE_VOLUMETRICS_BACK),
                                    .key  = std::string(WE_VOLUMETRICS_BACK),
                                    .type = rg::TexNode::TexType::Temp },
                /*write=*/true);
            builder.write(out);
        });
}

void emitVolumetricFrontPass(rg::RenderGraph& rgraph, Scene& scene, size_t light_idx,
                             ExtraInfoVolumetrics& /*extra*/) {
    auto& pl = scene.volumetricsConfig.per_light.at(light_idx);
    if (! pl.front_node) return;

    std::string passName = "volumetrics_front_" + std::to_string(light_idx);
    rgraph.addPass<CustomShaderPass>(
        passName,
        rg::PassNode::Type::CustomShader,
        [&pl](rg::RenderGraphBuilder& builder, CustomShaderPass::Desc& pdesc) {
            pdesc.node   = pl.front_node.get();
            pdesc.output = std::string(WE_VOLUMETRICS_LIGHT_BUFFER);
            // No force-clear: the light buffer accumulates additively across
            // every per-light front (or fullscreen) pass in the frame.
            pdesc.force_clear_output = false;
            // Sample the main scene depth + the back-face exit depth captured
            // by the matching back pass — the front-face shader integrates
            // between gl_FragCoord.z (front entry) and back depth (exit).
            pdesc.needsSceneDepth = true;
            pdesc.flipCullMode    = false;
            pdesc.textures        = { std::string(WE_VOLUMETRICS_BACK) };

            auto* back_in = builder.createTexNode(
                rg::TexNode::Desc { .name = std::string(WE_VOLUMETRICS_BACK),
                                    .key  = std::string(WE_VOLUMETRICS_BACK),
                                    .type = rg::TexNode::TexType::Temp });
            builder.read(back_in);

            auto* out = builder.createTexNode(
                rg::TexNode::Desc { .name = std::string(WE_VOLUMETRICS_LIGHT_BUFFER),
                                    .key  = std::string(WE_VOLUMETRICS_LIGHT_BUFFER),
                                    .type = rg::TexNode::TexType::Temp },
                /*write=*/true);
            builder.write(out);
        });
}

void emitVolumetricPerLight(rg::RenderGraph& rgraph, Scene& scene, size_t light_idx,
                            ExtraInfoVolumetrics& extra) {
    auto& pl = scene.volumetricsConfig.per_light.at(light_idx);
    if (! pl.light) return;
    emitVolumetricBackPass(rgraph, scene, light_idx, extra);
    // Camera-inside-the-volume swaps the front-face geometry pass for a
    // fullscreen quad — front-face rasterisation would clip when the eye sits
    // between the front face and the near plane.  is_inside_this_frame is
    // populated by Scene::TickVolumetricSelection at frame start.
    if (pl.is_inside_this_frame) {
        emitVolumetricFullscreenPass(rgraph, scene, light_idx, extra);
    } else {
        emitVolumetricFrontPass(rgraph, scene, light_idx, extra);
    }
}

void emitVolumetricFullscreenPass(rg::RenderGraph& rgraph, Scene& scene, size_t light_idx,
                                  ExtraInfoVolumetrics& /*extra*/) {
    auto& pl = scene.volumetricsConfig.per_light.at(light_idx);
    if (! pl.fullscreen_node) return;

    std::string passName = "volumetrics_fullscreen_" + std::to_string(light_idx);
    rgraph.addPass<CustomShaderPass>(
        passName,
        rg::PassNode::Type::CustomShader,
        [&pl](rg::RenderGraphBuilder& builder, CustomShaderPass::Desc& pdesc) {
            pdesc.node               = pl.fullscreen_node.get();
            pdesc.output             = std::string(WE_VOLUMETRICS_LIGHT_BUFFER);
            pdesc.force_clear_output = false;
            pdesc.needsSceneDepth    = true;
            // Fullscreen quad doesn't participate in depth testing — used when
            // the camera is INSIDE the light volume, where front-face rasters
            // would clip the eye.  Sample-only depth (still needed for the
            // integration cutoff at scene depth).
            pdesc.disableDepth = true;
            pdesc.flipCullMode = false;
            pdesc.textures     = { std::string(WE_VOLUMETRICS_BACK) };

            auto* back_in = builder.createTexNode(
                rg::TexNode::Desc { .name = std::string(WE_VOLUMETRICS_BACK),
                                    .key  = std::string(WE_VOLUMETRICS_BACK),
                                    .type = rg::TexNode::TexType::Temp });
            builder.read(back_in);

            auto* out = builder.createTexNode(
                rg::TexNode::Desc { .name = std::string(WE_VOLUMETRICS_LIGHT_BUFFER),
                                    .key  = std::string(WE_VOLUMETRICS_LIGHT_BUFFER),
                                    .type = rg::TexNode::TexType::Temp },
                /*write=*/true);
            builder.write(out);
        });
}

void emitVolumetricBlurPasses(rg::RenderGraph& rgraph, Scene& scene,
                              ExtraInfoVolumetrics& /*extra*/) {
    auto& cfg = scene.volumetricsConfig;
    if (cfg.blur_h_node) {
        emitVolumetricBlurOnePass(rgraph,
                                  "volumetrics_blur_h",
                                  WE_VOLUMETRICS_LIGHT_BUFFER,
                                  WE_VOLUMETRICS_LIGHT_BUFFER_B,
                                  cfg.blur_h_node.get());
    }
    if (cfg.blur_v_node) {
        // Vertical pass ping-pongs back into the primary light buffer so the
        // combine pass reads from a single canonical key.
        emitVolumetricBlurOnePass(rgraph,
                                  "volumetrics_blur_v",
                                  WE_VOLUMETRICS_LIGHT_BUFFER_B,
                                  WE_VOLUMETRICS_LIGHT_BUFFER,
                                  cfg.blur_v_node.get());
    }
}

void emitVolumetricCombinePass(rg::RenderGraph& rgraph, Scene& scene,
                               ExtraInfoVolumetrics& /*extra*/) {
    auto& cfg = scene.volumetricsConfig;
    if (! cfg.combine_node) return;

    rgraph.addPass<CustomShaderPass>(
        "volumetrics_combine",
        rg::PassNode::Type::CustomShader,
        [&cfg](rg::RenderGraphBuilder& builder, CustomShaderPass::Desc& pdesc) {
            pdesc.node     = cfg.combine_node.get();
            // Composite back onto the main scene RT.  The additive blend math
            // lives in the material attached separately to combine_node.
            pdesc.output   = std::string(SpecTex_Default);
            pdesc.textures = { std::string(WE_VOLUMETRICS_LIGHT_BUFFER) };

            auto* in = builder.createTexNode(
                rg::TexNode::Desc { .name = std::string(WE_VOLUMETRICS_LIGHT_BUFFER),
                                    .key  = std::string(WE_VOLUMETRICS_LIGHT_BUFFER),
                                    .type = rg::TexNode::TexType::Temp });
            builder.read(in);

            auto* out = builder.createTexNode(
                rg::TexNode::Desc { .name = std::string(SpecTex_Default),
                                    .key  = std::string(SpecTex_Default),
                                    .type = rg::TexNode::TexType::Temp },
                /*write=*/true);
            builder.write(out);
        });
}

void emitVolumetricChain(rg::RenderGraph& rgraph, Scene& scene,
                         const SceneCamera& cam, ExtraInfoVolumetrics& extra) {
    if (! scene.volumetricsConfig.enabled) return;
    if (scene.volumetricsConfig.per_light.empty()) return;
    if (cam.IsReflectY0()) return;
    // The chain samples the main scene-depth attachment to clip back-face
    // writes against opaque occluders and to bound the ray-march integration.
    // CustomShaderPass::prepare() disables the sampled-depth path under MSAA
    // (the multisampled depth image isn't directly sampleable in v1), which
    // would leave the corresponding descriptor binding empty and trip SPIR-V
    // reflection.  Skip the whole chain rather than emit passes guaranteed to
    // fail their descriptor pass.
    if (scene.msaaSamples > 1) return;

    for (size_t i = 0; i < scene.volumetricsConfig.per_light.size(); i++) {
        emitVolumetricPerLight(rgraph, scene, i, extra);
    }
    emitVolumetricBlurPasses(rgraph, scene, extra);
    emitVolumetricCombinePass(rgraph, scene, extra);
}

namespace
{

// Push one g_RenderVar<slot> value onto a node's material constValues.
// Three-tier null guard (node → mesh → material) so this stays a safe no-op
// before the per-light material attachment lands and when a node has lost
// its mesh (production: BuildVolumetricNodes always attaches a mesh, so the
// mesh-null branch is mostly belt-and-braces).
void writePerLightVarToNode(SceneNode* node, int slot, const std::array<float, 4>& v) {
    if (! node) return;
    auto* mesh = node->Mesh();
    if (! mesh) return;
    auto* mat = mesh->Material();
    if (! mat) return;
    std::string name                          = "g_RenderVar" + std::to_string(slot);
    mat->customShader.constValues[name]       = ShaderValue(v);
    mat->customShader.constValuesDirty        = true;
}

} // namespace

void PumpVolumetricFrame(Scene& scene, WPShaderValueUpdater& updater) {
    if (! scene.volumetricsConfig.enabled) return;
    if (scene.volumetricsConfig.per_light.empty()) return;

    // Step 1 — recompute is_inside_this_frame for every per-light entry from
    // the live camera/sphere intersection.  Read by emitVolumetricPerLight on
    // this same frame to pick front vs fullscreen.
    scene.TickVolumetricSelection();

    // Step 2 — write the per-light uniforms onto both the front AND
    // fullscreen materials.  Only one is rendered per frame, but writing
    // both keeps the unused node's uniforms ready for the frame after a
    // selection flip (avoids a one-frame uniform stutter when the camera
    // crosses the light boundary).  Back-pass writes depth only and has no
    // need for these slots.
    updater.UpdateVolumetricLightUniforms(
        [&scene](SceneLight* l, int slot, const std::array<float, 4>& v) {
            for (auto& pl : scene.volumetricsConfig.per_light) {
                if (pl.light != l) continue;
                writePerLightVarToNode(pl.front_node.get(), slot, v);
                writePerLightVarToNode(pl.fullscreen_node.get(), slot, v);
                break;
            }
        });
}

} // namespace wallpaper::vulkan
