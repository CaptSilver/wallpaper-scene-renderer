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

    struct ReflectionBlurConfig {
        std::vector<std::shared_ptr<SceneNode>> nodes;
        std::vector<std::string>                outputs;
    };
    ReflectionBlurConfig reflectionBlurConfig;

    std::shared_ptr<audio::AudioAnalyzer> audioAnalyzer;

    std::vector<TextLayerInfo>         textLayers;
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
    // Node lookup by ID for visibility updates
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
        std::vector<std::pair<i32, i32>> out;
        out.swap(m_pending_parent_changes);
        return out;
    }

    // Apply all queued parent changes against the live SceneNode tree.
    // Called from RenderHandler::DRAW.  Handles: child id miss (skip),
    // parent id miss (skip), parent_id == -1 (reattach to sceneGraph),
    // and old-parent extraction via SceneNode::ExtractChild + new-parent
    // re-attach via AppendChild (re-wires both m_parent and
    // m_visibility_parent).
    void ApplyPendingParentChanges() {
        auto pending = TakePendingParentChanges();
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
    void ApplyPendingChildSorts() {
        auto pending = TakePendingChildSorts();
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
    std::mutex                       m_pending_parent_mutex;
    std::vector<std::pair<i32, i32>> m_pending_parent_changes;
    std::mutex                       m_pending_sort_mutex;
    std::vector<std::pair<i32, i32>> m_pending_child_sorts;
};
} // namespace wallpaper
