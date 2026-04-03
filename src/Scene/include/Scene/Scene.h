#pragma once
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

#include "SceneTexture.h"
#include "SceneRenderTarget.h"
#include "SceneNode.h"
#include "SceneLight.hpp"

#include "Core/NoCopyMove.hpp"

#include <mutex>

namespace wallpaper
{
class ParticleSystem;
class IShaderValueUpdater;
class IImageParser;
class SceneImageEffectLayer;

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
};

struct SceneColorScript {
    i32                  id;
    SceneMaterial*       material;         // material owning g_Color4
    std::string          script;           // JavaScript source from color.script
    std::string          scriptProperties; // JSON string of script properties
    std::array<float, 3> initialColor;
};

struct VideoTextureInfo {
    std::string textureKey;    // key in Scene::textures / tex_cache
    std::string videoFilePath; // extracted MP4 temp file path
    i32         width { 0 };
    i32         height { 0 };
    SceneNode*  ownerNode { nullptr }; // node that uses this texture (for visibility gating)
};

struct ScenePropertyScript {
    i32                  id;
    std::string          property; // "visible", "origin", "scale", "angles", "alpha"
    std::string          script;
    std::string          scriptProperties; // JSON string
    std::string          layerName;        // name of the object for thisLayer.name
    bool                 initialVisible { true };
    std::array<float, 3> initialVec3 { 0, 0, 0 };
    float                initialFloat { 1.0f };
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

    std::vector<TextLayerInfo>       textLayers;
    std::vector<SceneColorScript>    colorScripts;
    std::vector<ScenePropertyScript> propertyScripts;
    std::vector<VideoTextureInfo>    videoTextures;

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
    // Layer name → node ID mapping for thisScene.getLayer()
    std::unordered_map<std::string, i32> nodeNameToId;
    // Layer name → initial transform state for JS proxy initialization
    struct LayerInitialState {
        std::array<float, 3> origin { 0, 0, 0 };
        std::array<float, 3> scale { 1, 1, 1 };
        std::array<float, 3> angles { 0, 0, 0 };
        std::array<float, 2> size { 0, 0 }; // layer pixel dimensions for hit testing
        bool                 visible { true };
    };
    std::unordered_map<std::string, LayerInitialState> layerInitialStates;

    // Layer name → ordered list of effect names for SceneScript getEffect()
    std::unordered_map<std::string, std::vector<std::string>> layerEffectNames;

    // Sound layer info for SceneScript play/stop/pause API (enumerateLayers)
    struct SoundLayerInfo {
        std::string name;
        float       initialVolume { 1.0f };
        bool        startsilent { false };
        void*       streamPtr { nullptr }; // WPSoundStream* (type-erased)
    };
    std::vector<SoundLayerInfo> soundLayers;

    // Sound volume scripts: evaluated at runtime to control per-stream volume
    struct SoundVolumeScript {
        std::string script;
        std::string scriptProperties;
        float       initialVolume { 1.0f };
        void*       streamPtr { nullptr }; // WPSoundStream* (type-erased)
    };
    std::vector<SoundVolumeScript> soundVolumeScripts;

    double elapsingTime { 0.0f }, frameTime { 0.0f };
    void   PassFrameTime(double t) {
        frameTime = t;
        elapsingTime += t;
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
};
} // namespace wallpaper
