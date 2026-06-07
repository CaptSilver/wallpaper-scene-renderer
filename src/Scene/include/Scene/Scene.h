#pragma once
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

#include "SceneTexture.h"
#include "SceneRenderTarget.h"
#include "SceneNode.h"
#include "SceneLight.hpp"
#include "WPPropertyAnimation.h"

#include "Core/NoCopyMove.hpp"

#include <mutex>
#include <atomic>

namespace wallpaper
{
class ParticleSystem;
class ParticleSubSystem;
class IShaderValueUpdater;
class IImageParser;
class SceneImageEffectLayer;
class WPPuppet; // forward — see WPPuppet.hpp; held by shared_ptr below.

namespace audio
{
class AudioAnalyzer;
}

namespace fs
{
class VFS;
}

struct TextLayerInfo {
    i32         id;
    std::string fontData; // cached raw font bytes
    std::string fontName; // font filename / VFS key (e.g. "Heavy.otf") — needed for
                          // SceneScript thisLayer.font round-trip and for VFS re-resolution
                          // when the JS proxy swaps the font at runtime.
    float       pointsize;
    i32         texWidth;
    i32         texHeight;
    i32         padding;
    std::string halign;
    std::string valign;
    std::string currentText;              // last rendered text
    std::string textureKey;               // key in textures / tex_cache
    std::string script;                   // JavaScript source from text.script
    std::string scriptProperties;         // JSON string of script properties
    std::string pointsizeUserProp;        // user property name controlling pointsize
    bool        pointsizeDirty { false }; // set when pointsize changes at runtime
    // Forces a re-rasterization with the current text when halign/valign/fontData
    // change at runtime (SceneScript thisLayer.horizontalalign/verticalalign/font/alignment).
    // Cleared after the next successful RenderText call in SceneWallpaper::CMD_DRAW.
    bool        textStyleDirty { false };
};

struct SceneColorScript {
    i32                  id;
    SceneMaterial*       material;         // material owning g_Color4
    std::string          script;           // JavaScript source from color.script
    std::string          scriptProperties; // JSON string of script properties
    std::array<float, 3> initialColor;
};

// Per-frame inline script attached to an effect-pass uniform via
// `effects[N].passes[M].constantshadervalues[X] = {script, scriptproperties, value}`.
// Game of Life (3453251764) Canvas object carries ~50 of these driving
// `mouseDown`, `mousePos`, `cmd`, `drawRadius`, `birthSurvive` etc. into
// the cell-paint shader; without dispatch the canvas was inert.
//
// The dispatch reuses the existing m_pending_effect_material_values drain
// in SceneWallpaper.cpp — script update() return values get packed into
// the same (nodeId, effectIdx, uniformName, floats) wire as
// thisLayer.getEffect(...).getMaterial().setValue(...).
struct SceneShaderValueScript {
    i32                id { -1 };         // owning layer id (Scene::nodeNameToId)
    i32                effectIdx { 0 };   // index into effects[] (0-based)
    std::string        uniformName;       // author-facing name (alias-resolved at drain)
    std::string        script;            // JavaScript source
    std::string        scriptProperties;  // JSON-serialized initial scriptProperties
    std::vector<float> initialValue;      // parsed scene.json `value` (scalar or vec)
    // Hint to the dispatch loop: the script's update(value) wants its arg
    // shaped as scalar (1), Vec2 (2), Vec3 (3), or Vec4 (4).  Inferred from
    // initialValue size; lets the dispatch present `update` with the right
    // typed arg without authors having to manually coerce.
    int                argShape { 1 };
};

struct VideoTextureInfo {
    std::string textureKey;    // key in Scene::textures / tex_cache
    std::string videoFilePath; // extracted MP4 temp file path
    i32         width { 0 };
    i32         height { 0 };
    // Nodes that sample this texture. The decoder pauses only when ALL owners
    // are invisible; otherwise at least one visible layer needs fresh frames.
    // Wallpaper 3276911872 has 8+ layers sharing one MP4 (morning5, day5,
    // dusk5, night5, and variants "1"/"2"/"3"/"4" under 精细 group) — any one
    // visible means the decoder must run.
    std::vector<SceneNode*> ownerNodes;
    // Back-compat for callers that want any owner pointer:
    SceneNode* ownerNode { nullptr };
};

struct ScenePropertyScript {
    // Where the script is attached on the object.  Object-attached scripts
    // (the common case) bind thisObject == thisLayer.  AnimationLayer
    // scripts — used by puppet wallpapers like Lucy (3521337568) to offset
    // their rigged animation layers at init — must bind thisObject to the
    // specific animation-layer proxy so setFrame/play land on it.
    enum class Attachment : uint8_t
    {
        Object         = 0,
        AnimationLayer = 1,
    };

    i32                  id;
    std::string          property; // "visible", "origin", "scale", "angles", "alpha"
    std::string          script;
    std::string          scriptProperties; // JSON string
    std::string          layerName;        // name of the parent object for thisLayer.name
    bool                 initialVisible { true };
    std::array<float, 3> initialVec3 { 0, 0, 0 };
    float                initialFloat { 1.0f };
    Attachment           attachment { Attachment::Object };
    i32                  animationLayerIndex { -1 }; // Attachment::AnimationLayer → 0-based idx
};
class Scene : NoCopy, NoMove {
public:
    Scene();
    ~Scene();

    std::unordered_map<std::string, SceneTexture>      textures;
    std::unordered_map<std::string, SceneRenderTarget> renderTargets;

    std::unordered_map<std::string, std::shared_ptr<SceneCamera>> cameras;
    std::unordered_map<std::string, std::vector<std::string>>     linkedCameras;

    std::vector<std::unique_ptr<SceneLight>> lights;

    std::shared_ptr<SceneNode>           sceneGraph;
    std::unique_ptr<IShaderValueUpdater> shaderValueUpdater;
    std::unique_ptr<IImageParser>        imageParser;
    std::unique_ptr<fs::VFS>             vfs;

    std::string scene_id { "unknown_id" };

    bool first_frame_ok { false };

    SceneMesh default_effect_mesh;
    // Proxy mesh for per-light volumetric scattering passes.  A 12-vertex / 20-
    // triangle unit icosphere centred at origin; the per-light pass scales and
    // translates this into a world-space light-volume bound.  Populated by
    // GenVolumeSphereMesh in WPSceneParser::InitContext, paralleling
    // default_effect_mesh.
    SceneMesh default_volume_sphere;

    std::unique_ptr<ParticleSystem> paritileSys;

    SceneCamera* activeCamera;

    i32                  ortho[2] { 1920, 1080 }; // w, h
    std::array<float, 3> clearColor { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> ambientColor { 0.2f, 0.2f, 0.2f };
    std::array<float, 3> skylightColor { 0.3f, 0.3f, 0.3f };

    // Opaque depth buffer owned by the Vulkan layer (shared across render passes)
    std::shared_ptr<void> depthBuffer;
    bool                  depthBufferCleared { false };

    // Separate depth buffer for reflection passes (avoids contaminating main depth)
    std::shared_ptr<void>           reflectionDepthBuffer;
    std::unordered_set<std::string> clearedRTs;

    // HDR content pipeline: when true, render targets use RGBA16F and FinPass tonemaps
    bool hdrContent { false };

    // 360° / skybox cubemap path: set true by WPSceneParser when at least one
    // image layer is flagged `is_skybox: true` in the scene JSON.  Currently a
    // detection-only stub — full rendering (cubemap upload + SkyboxPass with
    // worldDir sampling) is intentionally deferred until the rendergraph
    // infrastructure for cubemap views + a depth-disabled fullscreen pass is in
    // place.  Today, flagged scenes log a warning at scene-build and render via
    // the regular image-object path (single stretched face), which is no worse
    // than the pre-detection behaviour for ~0.1% of the workshop catalogue.
    // Non-skybox scenes are byte-identical because every consumer guards on
    // this flag.
    bool has_skybox { false };

    // Ids of image layers tagged `is_skybox: true`.  Empty when has_skybox is
    // false.  Surface kept narrow so downstream code can iterate authoritative
    // skybox layers without re-walking nodes.  Populated alongside has_skybox
    // in WPSceneParser::ParseImageObj.
    std::vector<i32> skyboxLayerIds;

    // Resolved per-scene post-processing tier ("ultra"/"displayhdr"/"medium"/
    // "low"/""), after the plugin-level override is applied on top of
    // scene.general.orthogonalprojection.postprocessing.  Stored on Scene so
    // downstream pipeline-build code (volumetric chain QUALITY combo) can
    // read the resolved tier without re-walking the override/scene-json
    // resolution logic that lives in WPSceneParser.cpp.  Empty when neither
    // the scene nor the override sets a value.
    std::string resolved_postprocessing;

    // MSAA: 1=off, 2/4/8=sample count (maps to VkSampleCountFlagBits)
    u32 msaaSamples { 1 };
    // Multisampled color images per render target (owned by Vulkan layer)
    std::unordered_map<std::string, std::shared_ptr<void>> msaaColorImages;
    // Multisampled depth buffers (main + reflection)
    std::shared_ptr<void> msaaDepthBuffer;
    std::shared_ptr<void> msaaReflectionDepthBuffer;

    struct BloomConfig {
        bool                                    enabled { false };
        float                                   strength { 2.0f };
        float                                   threshold { 0.65f };
        std::vector<std::shared_ptr<SceneNode>> nodes;
        std::vector<std::string>                outputs;
    };
    BloomConfig bloomConfig;

    // Volumetric fog scene-level config — mirror of BloomConfig.  Populated by
    // WPSceneParser at end-of-parse based on the per-light castsVolumetrics()
    // predicate.  `enabled` short-circuits the 6-pass chain when no light wants
    // fog; `globalDensityMultiplier` is a stub for a possible scene-global
    // density modulator (a later integration step may plug it through, or
    // remove the field if no source surfaces).
    struct VolumetricsConfig {
        bool  enabled                 { false };
        float globalDensityMultiplier { 1.0f };
        // QUALITY combo emitted into the per-light volumetric materials,
        // sourced from the per-wallpaper post-processing tier.  Default 2
        // ("medium" tier); the ray-march step count for each tier is
        // determined inside the volumetricsfront.frag shader.  Resolved at
        // scene-build time by BuildVolumetricNodes from
        // Scene::resolved_postprocessing.
        uint32_t quality { 2 };
        // Per-volumetric-light state: the light pointer (non-owning — Scene
        // owns the unique_ptr), the three pre-built SceneNodes that the per-
        // light passes rasterise (back proxy, front proxy, fullscreen
        // alternate for camera-inside-volume), and the per-frame
        // camera-inside flag set by TickVolumetricSelection.  Populated by
        // BuildVolumetricNodes at scene-build time; iterated by the volumetric
        // chain emission code at render-graph-build time.
        struct PerLight {
            SceneLight*                light { nullptr };
            std::shared_ptr<SceneNode> back_node;
            std::shared_ptr<SceneNode> front_node;
            std::shared_ptr<SceneNode> fullscreen_node;
            bool                       is_inside_this_frame { false };
        };
        std::vector<PerLight>      per_light;
        // Global single-instance nodes for the post-per-light chain stages.
        std::shared_ptr<SceneNode> blur_h_node;
        std::shared_ptr<SceneNode> blur_v_node;
        std::shared_ptr<SceneNode> combine_node;
    };
    VolumetricsConfig volumetricsConfig;

    struct ReflectionBlurConfig {
        std::vector<std::shared_ptr<SceneNode>> nodes;
        std::vector<std::string>                outputs;
    };
    ReflectionBlurConfig reflectionBlurConfig;

    std::shared_ptr<audio::AudioAnalyzer> audioAnalyzer;
    // true iff some particle subsystem is audio-reactive
    // (audioprocessingmode != 0).  Set once at scene build; lets the render
    // thread skip the per-frame emit-rate map walk + spectrum span fetch on
    // non-reactive scenes without re-scanning particleSubByNodeId.
    bool hasAudioReactiveParticles { false };

    std::vector<TextLayerInfo>         textLayers;
    // Transform-only helper nodes (no mesh, never traversed) that carry a
    // baked world transform so a non-effect child can recover its parent's
    // position after that parent's live node was reset to identity for its own
    // effect base pass.  Owned here for scene lifetime; children hold a raw
    // m_parent pointer into them (see WPSceneParser attachTextNodeToScene).
    std::vector<std::shared_ptr<SceneNode>> ownedProxyNodes;

    // Live parent-tracking for attachment/effect-chain children.  An attached
    // child's world is `parentWorld * boneAttOffset * childLocal`; we bake the
    // parse-time value into a proxy node, but a parent whose transform is
    // script-driven at runtime (e.g. a puppet head rotating toward the cursor)
    // then leaves the baked proxy stale and the child detaches.  Each link lets
    // the draw loop recompose the proxy every frame from the parent's *current*
    // world (matching Wallpaper Engine, which never caches the attachment).
    // `offset` is the constant `boneWorld * attachment` factor (identity when
    // the child has no bone attachment).
    struct AttachmentProxyLink {
        SceneNode*      proxy { nullptr };  // proxy whose world is refreshed
        i32             parent_id { -1 };   // live parent to read the world from
        i32             child_id { -1 };    // child node (to re-dirty its subtree)
        Eigen::Matrix4d offset { Eigen::Matrix4d::Identity() };
        int             depth { 0 };        // parent-chain depth; refresh parents first
    };
    std::vector<AttachmentProxyLink> attachmentProxyLinks;

    // Keep-alive owner for proxy nodes used as the TRANSFORM parent of plain
    // (effect-less) children whose real parent node was reset to identity for
    // its own effect chain.  Effect-children anchor their proxy on the
    // SceneImageEffectLayer; plain children have no such owner, so the Scene
    // holds them here (the AttachmentProxyLink above stores only a raw ptr).
    std::vector<std::shared_ptr<SceneNode>> attachmentProxyKeepAlive;

    std::vector<SceneColorScript>      colorScripts;
    std::vector<ScenePropertyScript>   propertyScripts;
    std::vector<SceneShaderValueScript> shaderValueScripts;
    std::vector<VideoTextureInfo>      videoTextures;

    // Runtime user property bindings for instant updates
    struct UserPropVisibility {
        SceneNode*  node;
        bool        defaultVisible; // original value without override
        std::string conditionValue; // combo: visible when prop == this value (empty = boolean)
        std::string rawVisibleJson; // raw JSON of "visible" field for re-resolution
    };
    struct UserPropUniform {
        SceneMaterial* material;
        std::string    uniformName; // glsl uniform name (e.g. "g_UserAlpha")
    };
    std::unordered_map<std::string, std::vector<UserPropVisibility>> userPropVisBindings;
    std::unordered_map<std::string, std::vector<UserPropUniform>>    userPropUniformBindings;
    // Node lookup by ID for visibility updates and SceneScript getLayer
    // resolution.
    //
    // LIFETIME INVARIANT: built once during parse (WPSceneParser populates
    // entries as nodes are constructed).  Never erased or cleared at
    // runtime -- destruction of the Scene shared_ptr frees the entries
    // atomically along with the SceneNodes themselves (raw pointers alias
    // owners elsewhere in Scene).  Per-frame iteration is safe for the
    // duration the Scene is observable to the render thread.  The
    // SceneScript thisScene.destroyLayer shim is a JS-side visibility flip
    // + pool bookkeeping that does NOT touch this map.  If a future change
    // adds a runtime erase path, switch to weak_ptr or coordinated-erase
    // before landing that change -- the regression tests under
    // TEST_SUITE("ParticleSubByNodeId Lifetime") will fail loudly first.
    std::unordered_map<i32, SceneNode*> nodeById;
    // Node ID → effect layer for redirecting property script transform updates.
    // Nodes with effect chains need transforms applied to the final composite
    // node rather than the world node (which stays at identity for base render).
    std::unordered_map<i32, SceneImageEffectLayer*> nodeEffectLayerMap;
    // Node ID → puppet for SceneScript thisLayer.getBoneIndex(name) lookups.
    // Mirrors WPSceneParser's transient context.node_puppet so the runtime can
    // resolve attachment names to bone indices for cross-puppet rigging.
    std::unordered_map<i32, std::shared_ptr<WPPuppet>> nodePuppetMap;
    // Layer name → node ID mapping for thisScene.getLayer()
    std::unordered_map<std::string, i32> nodeNameToId;

    // Image-layer id → static base texture name (e.g. "ynight_tm2k").  Populated
    // during ParseImageObj from `wpimgobj.material.textures.front()`.  Read by
    // ParseSpecTexName to route `_rt_imageLayerComposite_<id>_b` references —
    // an undocumented WE convention some custom effects use to mean "my parent
    // layer's pre-effect content" — to the layer's static base instead of the
    // post-effect _rt_link_<id> that the `_a` suffix maps to.  Without this,
    // a layer whose own effect samples its own `_b` reads back its own previous
    // post-effect output (cumulative UV drift each frame).  Driver: Real-Time
    // Earth (3557068717) id 424's `____________` UV-scroll effect samples
    // `_rt_imageLayerComposite_424_b` to phase-shift its source by UTC time.
    std::unordered_map<i32, std::string> imgIdToSourceTexture;
    // JSON-authored parent for each object ID — populated at parse time
    // straight from scene.json's `parent` field.  Used by
    // SerializeLayerInitialStates to emit `pn` (parent name) when the
    // runtime SceneNode hierarchy has been rewired through effect-RT
    // wrappers and no longer reflects the JSON parent.  Background under
    // `Text Container` in Floating Cat (3367988661) is the driver: the
    // runtime SceneNode.Parent() walks up the effect-RT chain to the scene
    // root, while the script-visible parent should still be `Text Container`.
    std::unordered_map<i32, i32> jsonParentId;

    // Per-node SceneScript-driven sprite frame override.  Populated by
    // SceneWallpaper::setLayerSpriteFrame when JS writes
    // `thisLayer.getTextureAnimation().setFrame(N)`.  The per-frame uniform
    // updater (WPShaderValueUpdater::UpdateUniforms) consults this map for
    // each sprite it processes — when an entry exists for the owning node,
    // the sprite is pinned to that frame via SpriteAnimation::SetManualFrame
    // (suppressing auto-advance); absent entries restore auto-advance.
    // Solves Game of Life (3453251764) color/tool buttons cycling through
    // their 3-frame sprite sheets at the texture's authored rate.
    // The bool is a "wants-manual" toggle (false = explicit auto-advance
    // restore), the i32 is the desired frame index when manual is true.
    std::unordered_map<i32, std::pair<bool, i32>> nodeSpriteFrame;

    // Per-node sprite playback snapshot, written each render tick by
    // WPShaderValueUpdater after the sprite advances, read by the
    // SceneScript bridge for `thisLayer.getTextureAnimation()`'s frameCount /
    // duration / getFrame() / isPlaying().  Without a live read-back, the JS
    // proxy hardcoded frameCount=1 and currentFrame=0, which made authoring
    // patterns like `frame === ani.frameCount - 1` (Rella firework script
    // 3363252053) fire every tick and tear through the pause/resume cycle.
    // Populated from the first sprite of the first pass; multiple passes for
    // the same node share authored frametimes, so the representative sprite
    // is in sync with the rest within one tick.
    struct NodeSpriteSnapshot {
        u32   numFrames { 0 };
        u32   currentFrame { 0 };
        float duration { 0.0f }; // sum of authored frametimes, seconds
        bool  isManualPin { false };
    };
    std::unordered_map<i32, NodeSpriteSnapshot> nodeSpriteSnapshot;
    mutable std::mutex                          nodeSpriteSnapshotMutex;
    // Consumer-gate flag.  Latched true by the first call to
    // getLayerSpriteSnapshot (thisLayer.getTextureAnimation() bridge).
    // The per-frame producer in WPShaderValueUpdater skips publication
    // when this is false.  Sticky-on within a scene; resets implicitly
    // on scene swap (a new Scene is constructed).  Mirrors the
    // WorldCacheGate pattern.
    mutable std::atomic<bool>                   needsSpriteSnapshot { false };

    // Layer name → initial transform state for JS proxy initialization
    struct LayerInitialState {
        std::array<float, 3> origin { 0, 0, 0 };
        std::array<float, 3> scale { 1, 1, 1 };
        std::array<float, 3> angles { 0, 0, 0 };
        std::array<float, 2> size { 0, 0 }; // layer pixel dimensions for hit testing
        // Per-layer parallax depth (default 0 = no parallax).  Cursor hit-
        // testing mirrors WPShaderValueUpdater's MVP adjustment so clicks
        // land where the parallax-shifted layer is actually rendered.
        std::array<float, 2> parallaxDepth { 0, 0 };
        bool                 visible { true };
        // World-space origin and scale, baked at parse time from the full
        // parent chain (including image-object ancestors).  Cursor hit-test
        // uses these to compare against world-space cursor coordinates;
        // without them the test compared a world cursor against a local
        // origin and missed every parented button (Game of Life
        // 3453251764 Tool Selection / Color / Blueprint buttons all clicked
        // through to whatever was rendered behind them).  Stable for the
        // lifetime of the scene — scripts that mutate origin/scale at
        // runtime accept hit-test drift until next setSource.
        std::array<float, 3> worldOrigin { 0, 0, 0 };
        std::array<float, 3> worldScale { 1, 1, 1 };
    };
    std::unordered_map<std::string, LayerInitialState> layerInitialStates;

    // Layer name → ordered list of effect names for SceneScript getEffect()
    std::unordered_map<std::string, std::vector<std::string>> layerEffectNames;

    // SceneScript dynamic-asset pools.  For each path passed to
    // engine.registerAsset(...), a pre-allocated pool of hidden scene nodes is
    // created at parse time.  thisScene.createLayer(asset) pops one off the
    // pool (sets visible=true), and destroyLayer pushes it back.  The vector
    // stores the pool layer names; look them up via nodeNameToId /
    // layerInitialStates like any other named layer.
    std::unordered_map<std::string, std::vector<std::string>> assetPools;

    // Node ID → particle subsystem, for pool-backed particle assets.  When
    // the render thread sees a pool particle node's visibility flip from
    // false to true (via createLayer), it calls Reset() on the matching
    // subsystem so the burst emitter re-fires.
    //
    // LIFETIME INVARIANT: built once during parse (WPSceneParser at two
    // sites).  Never erased or cleared at runtime.  The raw pointer aliases
    // a unique_ptr in ParticleSystem::subsystems, also parse-only filled.
    // Destruction of the Scene frees both atomically.
    //
    // The SceneScript thisScene.destroyLayer shim is a JS-side visibility
    // flip + pool bookkeeping -- it does NOT touch this map nor delete the
    // subsystem; the subsystem stays alive (and silent -- IsVisible() ==
    // false short-circuits draw).  Callers iterating this map per-frame
    // can rely on raw-pointer stability for the duration the Scene
    // shared_ptr is observable to the render thread.
    //
    // The `if (!sub || ...)` guard at the emit-rate scan site is defensive
    // against a future state where some code path nulls a slot; today it
    // never fires.  Tests under TEST_SUITE("ParticleSubByNodeId Lifetime")
    // pin this contract -- if they fail, the invariant has been broken and
    // the consumer-side iteration must switch to weak_ptr or coordinated-
    // erase before landing the change that broke it.
    std::unordered_map<i32, ParticleSubSystem*> particleSubByNodeId;

    // Sound layer info for SceneScript play/stop/pause API (enumerateLayers)
    struct SoundLayerInfo {
        std::string name;
        float       initialVolume { 1.0f };
        bool        startsilent { false };
        void*       streamPtr { nullptr }; // WPSoundStream* (type-erased)
    };
    std::vector<SoundLayerInfo> soundLayers;

    // Sound volume scripts: evaluated at runtime to control per-stream volume
    struct VolumeKeyframe {
        float frame { 0 };
        float value { 0 };
    };
    struct VolumeAnimationData {
        std::string                 name;
        std::string                 mode { "loop" };
        float                       fps { 30.0f };
        float                       length { 0 };
        std::vector<VolumeKeyframe> keyframes;
    };
    struct SoundVolumeScript {
        std::string         script;
        std::string         scriptProperties;
        std::string         layerName; // sound layer name for thisLayer binding
        float               initialVolume { 1.0f };
        void*               streamPtr { nullptr }; // WPSoundStream* (type-erased)
        bool                hasAnimation { false };
        VolumeAnimationData animation;
    };
    std::vector<SoundVolumeScript> soundVolumeScripts;

    // Keyframe property animations keyed by scene-node id.  One vector entry
    // per named animation (uniqueness within a node's vector is enforced by
    // WE authoring; we don't deduplicate).  Ticked each frame on the render
    // thread; values written directly into the node's material const data.
    std::unordered_map<i32, std::vector<PropertyAnimation>> nodePropertyAnimations;

    double elapsingTime { 0.0f }, frameTime { 0.0f };
    void   PassFrameTime(double t) {
        frameTime = t;
        elapsingTime += t;
    }

    // Serialize all layer initial states (origin/scale/angles/visible/size/
    // parallaxDepth/effects/parent-name) to a JSON string consumed by the
    // SceneScript JS proxy initializer (_layerInitStates).  Implemented in
    // Scene.cpp to keep nlohmann/json out of this header.
    std::string SerializeLayerInitialStates() const;

    // Non-throwing render-target lookup.  Returns nullptr when `name` is
    // absent instead of throwing std::out_of_range like renderTargets.at().
    // Used by the render passes (CustomShaderPass / CopyPass) so a malformed
    // scene that references a missing RT logs+skips rather than escaping an
    // exception onto the render worker thread (see Looper try/catch).  Inline
    // so no new translation-unit symbol / plugin .so linkage change.
    SceneRenderTarget* tryGetRenderTarget(const std::string& name) {
        auto it = renderTargets.find(name);
        return it != renderTargets.end() ? &it->second : nullptr;
    }
    const SceneRenderTarget* tryGetRenderTarget(const std::string& name) const {
        auto it = renderTargets.find(name);
        return it != renderTargets.end() ? &it->second : nullptr;
    }

    void UpdateLinkedCamera(const std::string& name) {
        if (linkedCameras.count(name) != 0) {
            auto& cams = linkedCameras.at(name);
            for (auto& cam : cams) {
                if (cameras.count(cam) != 0) {
                    cameras.at(cam)->Clone(*cameras.at(name));
                    cameras.at(cam)->Update();
                }
            }
        }
    }

    // Pointers to all lights where SceneLight::castsVolumetrics() is currently
    // true.  Walks lights[] once; cheap enough to call at scene-build and at
    // the start of each frame.  Pointers remain valid for the lifetime of
    // Scene.
    std::vector<SceneLight*> volumetricLights() const;

    // Update per_light[i].is_inside_this_frame based on the camera's current
    // world position vs each light's world origin + radius.  Called from the
    // render-thread per-frame tick before the volumetric uniform pump.  No-op
    // when volumetricsConfig.per_light is empty or activeCamera is null.
    // A camera AT exactly the surface (distance == radius) is treated as
    // outside so the front-face geometry path stays in control.
    void TickVolumetricSelection();

    // ===================================================================
    // Pending parent-change queue — drained at the start of
    // RenderHandler::CMD_DRAW.  Same pattern as m_pending_material_values.
    // Each pair is (child_node_id, new_parent_id).  parent_id == -1 means
    // "reattach to scene root".  Mutex-guarded.
    // ===================================================================
    void QueueParentChange(i32 child_id, i32 parent_id) {
        std::lock_guard<std::mutex> lk(m_pending_parent_mutex);
        m_pending_parent_changes.emplace_back(child_id, parent_id);
    }

    std::vector<std::pair<i32, i32>> TakePendingParentChanges() {
        std::lock_guard<std::mutex> lk(m_pending_parent_mutex);
        return TakePendingParentChanges_locked();
    }

    // Apply all queued parent changes against the live SceneNode tree.
    // Called from RenderHandler::DRAW.  Handles: child id miss (skip),
    // parent id miss (skip), parent_id == -1 (reattach to sceneGraph),
    // and old-parent extraction via SceneNode::ExtractChild + new-parent
    // re-attach via AppendChild (re-wires both m_parent and
    // m_visibility_parent).
    //
    // m_pending_parent_mutex is held across the WHOLE drain (not just the
    // queue swap), because the mutation rewrites SceneNode::m_parent and the
    // m_children lists that the cross-thread reader (ResolveParentNodeId, on
    // the QML thread) walks under the same lock — otherwise that walk would
    // see a torn parent pointer / a std::list mid-mutation.
    void ApplyPendingParentChanges() {
        std::lock_guard<std::mutex> lk(m_pending_parent_mutex);
        auto                        pending = TakePendingParentChanges_locked();
        for (auto& [child_id, parent_id] : pending) {
            auto child_it = nodeById.find(child_id);
            if (child_it == nodeById.end()) continue;
            SceneNode* child_raw = child_it->second;
            if (! child_raw) continue;

            SceneNode* new_parent = nullptr;
            if (parent_id == -1) {
                new_parent = sceneGraph.get();
            } else {
                auto parent_it = nodeById.find(parent_id);
                if (parent_it == nodeById.end()) continue;
                new_parent = parent_it->second;
            }
            if (! new_parent) continue;

            SceneNode* old_parent = child_raw->Parent();
            if (! old_parent) continue;
            auto child_sp = old_parent->ExtractChild(child_raw);
            if (! child_sp) continue;
            new_parent->AppendChild(child_sp);
        }
    }

    // Resolve the scene-graph parent of `childId` to its node id (or -1 if the
    // child is unknown, has no parent, or the parent isn't in nodeById).
    // Taken under m_pending_parent_mutex so it never races
    // ApplyPendingParentChanges / ApplyPendingChildSorts rewriting m_parent and
    // the children lists on the render thread.  Returns a plain value type so
    // no SceneNode pointer escapes the lock (used by SceneScript
    // getBoneIndex's parent-walk on the QML thread).
    i32 ResolveParentNodeId(i32 childId) const {
        std::lock_guard<std::mutex> lk(m_pending_parent_mutex);
        auto                        cit = nodeById.find(childId);
        if (cit == nodeById.end() || ! cit->second) return -1;
        SceneNode* parent = cit->second->Parent();
        if (! parent) return -1;
        for (auto& [pid, pnode] : nodeById) {
            if (pnode == parent) return pid;
        }
        return -1;
    }

    // ===================================================================
    // Pending child-sort queue — drained alongside parent-change queue at
    // the start of RenderHandler::CMD_DRAW.  Each pair is
    // (child_node_id, target_index).  target_index clamps to
    // [0, parent->children.size()-1].  Mutex-guarded.
    //
    // Used by SceneScript thisScene.sortLayer(layer, index) so wallpapers
    // (Blue Archive 2764537029 visualizer) can keep dynamically-spawned
    // layers at a specific depth slot relative to siblings.
    // ===================================================================
    void QueueChildSort(i32 child_id, i32 target_index) {
        std::lock_guard<std::mutex> lk(m_pending_sort_mutex);
        m_pending_child_sorts.emplace_back(child_id, target_index);
    }

    std::vector<std::pair<i32, i32>> TakePendingChildSorts() {
        std::lock_guard<std::mutex> lk(m_pending_sort_mutex);
        std::vector<std::pair<i32, i32>> out;
        out.swap(m_pending_child_sorts);
        return out;
    }

    // Apply queued child sorts.  Each sort extracts the child from its
    // current parent's children list and re-inserts at target_index
    // (clamped).  Unknown ids and orphan children skip silently.
    //
    // The queue swap uses m_pending_sort_mutex (released before we take the
    // tree lock — no nesting), then the tree mutation is held under
    // m_pending_parent_mutex so it shares the same exclusion as
    // ApplyPendingParentChanges and ResolveParentNodeId (it rewrites m_parent
    // and m_children just like AppendChild does).
    void ApplyPendingChildSorts() {
        auto                        pending = TakePendingChildSorts();
        std::lock_guard<std::mutex> lk(m_pending_parent_mutex);
        for (auto& [child_id, target_index] : pending) {
            auto child_it = nodeById.find(child_id);
            if (child_it == nodeById.end()) continue;
            SceneNode* child_raw = child_it->second;
            if (! child_raw) continue;
            SceneNode* parent = child_raw->Parent();
            if (! parent) continue;
            auto child_sp = parent->ExtractChild(child_raw);
            if (! child_sp) continue;
            parent->InsertChildAt(child_sp, target_index);
        }
    }

private:
    // No-lock queue swap; caller must already hold m_pending_parent_mutex.
    std::vector<std::pair<i32, i32>> TakePendingParentChanges_locked() {
        std::vector<std::pair<i32, i32>> out;
        out.swap(m_pending_parent_changes);
        return out;
    }

    // mutable: ResolveParentNodeId is const but must lock to read the tree.
    mutable std::mutex               m_pending_parent_mutex;
    std::vector<std::pair<i32, i32>> m_pending_parent_changes;
    std::mutex                       m_pending_sort_mutex;
    std::vector<std::pair<i32, i32>> m_pending_child_sorts;
};
} // namespace wallpaper
