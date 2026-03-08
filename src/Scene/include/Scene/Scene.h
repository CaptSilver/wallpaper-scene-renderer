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

namespace fs
{
class VFS;
}

struct TextLayerInfo {
    i32         id;
    std::string fontData;       // cached raw font bytes
    float       pointsize;
    i32         texWidth;
    i32         texHeight;
    i32         padding;
    std::string halign;
    std::string valign;
    std::string currentText;    // last rendered text
    std::string textureKey;     // key in textures / tex_cache
    std::string script;         // JavaScript source from text.script
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

    // Opaque depth buffer owned by the Vulkan layer (shared across render passes)
    std::shared_ptr<void> depthBuffer;
    bool                  depthBufferCleared { false };

    // Separate depth buffer for reflection passes (avoids contaminating main depth)
    std::shared_ptr<void> reflectionDepthBuffer;
    std::unordered_set<std::string> clearedRTs;

    struct BloomConfig {
        bool                                     enabled { false };
        float                                    strength { 2.0f };
        float                                    threshold { 0.65f };
        std::vector<std::shared_ptr<SceneNode>>  nodes;
        std::vector<std::string>                 outputs;
    };
    BloomConfig bloomConfig;

    struct ReflectionBlurConfig {
        std::vector<std::shared_ptr<SceneNode>> nodes;
        std::vector<std::string>                outputs;
    };
    ReflectionBlurConfig reflectionBlurConfig;

    std::vector<TextLayerInfo> textLayers;

    // Runtime user property bindings for instant updates
    struct UserPropVisibility {
        SceneNode* node;
        bool       defaultVisible; // original value without override
    };
    struct UserPropUniform {
        SceneMaterial* material;
        std::string    uniformName; // glsl uniform name (e.g. "g_UserAlpha")
    };
    std::unordered_map<std::string, std::vector<UserPropVisibility>> userPropVisBindings;
    std::unordered_map<std::string, std::vector<UserPropUniform>>    userPropUniformBindings;
    // Node lookup by ID for visibility updates
    std::unordered_map<i32, SceneNode*> nodeById;

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
