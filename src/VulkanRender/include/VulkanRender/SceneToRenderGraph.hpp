#pragma once
#include <memory>

namespace wallpaper
{

class Scene;
namespace rg
{
class RenderGraph;
}

std::unique_ptr<rg::RenderGraph> sceneToRenderGraph(Scene&);

namespace vulkan
{

// Emit the equirect skybox background pass onto rgraph.  Writes _rt_default
// (the base other layers composite onto) and reads scene.skyboxTexKey.
// No-op unless all three hold: scene.has_skybox, a non-empty skyboxLayerIds,
// and a non-empty skyboxTexKey — so non-skybox scenes emit nothing and the
// flat render path stays byte-identical.  Called before the per-node walk in
// sceneToRenderGraph so the skybox is the first writer of _rt_default.
void emitSkyboxPass(rg::RenderGraph& rgraph, Scene& scene);

// True when a skybox scene also carries foreground renderable nodes (more
// renderable nodes than skybox layers).  Those foreground nodes render flat
// (no perspective camera behind the panorama); the emit path logs a one-time
// diagnostic.  Pure predicate so the decision is unit-testable without the log.
bool skyboxHasForegroundNodes(const Scene& scene);

} // namespace vulkan
} // namespace wallpaper
