#pragma once
#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "RenderGraph/RenderGraph.hpp"

#include <cstdint>
#include <string_view>

namespace wallpaper
{
class WPShaderValueUpdater;
namespace wpscene
{
// Forward-declared so VolumetricChain.hpp doesn't drag the full WPMaterial
// header — which transitively includes nlohmann/json — into every consumer
// of the volumetric chain.  Callers of the build*Material helpers
// must include wpscene/WPMaterial.h themselves to actually touch the
// returned object's fields.
class WPMaterial;
}
}

namespace wallpaper::vulkan
{

// Per-emission context for the volumetric chain.  Pass by ref to the
// emit* functions.  Holds back-references to Scene + RenderGraph for the
// helpers that need both (e.g. global blur).  Future fields may carry pass
// IDs for later linkup.
struct ExtraInfoVolumetrics {
    Scene*           scene { nullptr };
    rg::RenderGraph* rgraph { nullptr };
};

// Build the per-light + global SceneNodes for the volumetric scattering
// chain.  Call once at end-of-parse; re-running replaces per-light nodes but
// preserves already-constructed global nodes (not safe for incremental
// rebuild of a partially-mutated scene).
//
// Reads:
//   scene.volumetricsConfig.per_light (must be populated with .light fields
//     beforehand)
//   scene.default_volume_sphere       (per-light back/front proxy mesh)
//   scene.default_effect_mesh         (fullscreen quad + global pass mesh)
//
// Writes:
//   scene.volumetricsConfig.per_light[i].{back_node, front_node, fullscreen_node}
//   scene.volumetricsConfig.{blur_h_node, blur_v_node, combine_node}
//   scene.renderTargets[_rt_volumetricsBack/LightBuffer/LightBufferB] — half-res RGBA16F
//
// Per-light nodes carry a transform sized to the light's radius and
// translated to the light's world origin.  Material attachment is the
// caller's responsibility.
//
// No-op when scene.volumetricsConfig.per_light is empty.
void BuildVolumetricNodes(Scene& scene);

// ---------------------------------------------------------------------------
// Volumetric chain render-graph emitters.
//
// Each emit* function appends one (or two) CustomShaderPass nodes onto the
// supplied RenderGraph.  All are no-ops when the corresponding SceneNode in
// scene.volumetricsConfig is null (e.g. lights with castsVolumetrics() == false
// were skipped at BuildVolumetricNodes time).
// ---------------------------------------------------------------------------

void emitVolumetricBackPass(rg::RenderGraph& rgraph, Scene& scene, size_t light_idx,
                            ExtraInfoVolumetrics& extra);
void emitVolumetricFrontPass(rg::RenderGraph& rgraph, Scene& scene, size_t light_idx,
                             ExtraInfoVolumetrics& extra);
void emitVolumetricFullscreenPass(rg::RenderGraph& rgraph, Scene& scene, size_t light_idx,
                                  ExtraInfoVolumetrics& extra);
void emitVolumetricPerLight(rg::RenderGraph& rgraph, Scene& scene, size_t light_idx,
                            ExtraInfoVolumetrics& extra);
void emitVolumetricBlurPasses(rg::RenderGraph& rgraph, Scene& scene,
                              ExtraInfoVolumetrics& extra);
void emitVolumetricCombinePass(rg::RenderGraph& rgraph, Scene& scene,
                               ExtraInfoVolumetrics& extra);

// Top-level entry point: emits the full volumetric chain (per-light back/front,
// global blur ping-pong, additive combine) onto rgraph.  Hard-skipped under:
//   - scene.volumetricsConfig.enabled == false
//   - scene.volumetricsConfig.per_light empty
//   - cam.IsReflectY0() (reflection passes use a different depth buffer + the
//     reflected fog would look wrong)
//   - scene.msaaSamples > 1 (sampled-depth path disabled under MSAA in v1; the
//     chain's per-light passes depend on scene-depth sampling, so emitting
//     them would trip the descriptor-set reflection check)
// Caller is responsible for ensuring Scene::TickVolumetricSelection() has run
// this frame so per-light is_inside_this_frame is current.
void emitVolumetricChain(rg::RenderGraph& rgraph, Scene& scene,
                         const SceneCamera& cam, ExtraInfoVolumetrics& extra);

// Per-frame volumetric tick:
//   1. TickVolumetricSelection() — recompute per-light is_inside_this_frame
//      from the active camera vs each light-world-origin sphere.
//   2. UpdateVolumetricLightUniforms(...) — push g_RenderVar0..4 onto the
//      per-light material's customShader.constValues.
//
// Call once per frame BEFORE the render-graph executes.  No-op when the
// volumetric chain is disabled or no volumetric lights are configured.
//
// The default writer targets BOTH the front AND fullscreen per-light
// materials so the unused one is still ready when is_inside_this_frame
// flips on the next frame.  Back-pass material writes depth only and does
// not need these uniforms.
//
// `updater` must be the same shader-value updater that owns the per-light
// uniform packing logic — pass the dynamic-cast'd WPShaderValueUpdater
// from scene.shaderValueUpdater.get().
void PumpVolumetricFrame(Scene& scene, WPShaderValueUpdater& updater);

// Map the resolved per-scene post-processing tier string to the QUALITY combo
// value baked into every volumetric material at compile time.  WE's
// volumetricsfront.frag branches on QUALITY for the ray-march step count:
// 4 = "ultra" / "displayhdr" (highest), 2 = "medium" / unset (default),
// 1 = "low" (cheapest).  Any unrecognised tier resolves to the medium default
// so misconfigured wallpapers still render rather than failing closed.
uint32_t volumetric_quality_from_pp_tier(std::string_view pp);

// Pure-data WPMaterial builders for the six volumetric chain passes.  Each
// returns a fully populated wpscene::WPMaterial with the shader name, blend
// mode, cull mode, depth state, textures, and combos that the LoadMaterial
// helper in WPSceneParser.cpp needs to compile the shader.  Exposed for
// unit testing of the QUALITY plumbing and the material spec table; the
// attach loop in WPSceneParser.cpp calls these and then runs LoadMaterial.
wpscene::WPMaterial buildVolumetricBackMaterial(uint32_t quality);
wpscene::WPMaterial buildVolumetricFrontMaterial(uint32_t quality);
wpscene::WPMaterial buildVolumetricFullscreenMaterial(uint32_t quality);
wpscene::WPMaterial buildVolumetricBlurMaterial(int vertical);
wpscene::WPMaterial buildVolumetricCombineMaterial();

} // namespace wallpaper::vulkan
