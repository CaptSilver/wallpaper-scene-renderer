#pragma once
#include "WPJson.hpp"
#include <nlohmann/json.hpp>
#include "WPMaterial.h"
#include <vector>
#include "WPPuppet.hpp"
#include "WPPropertyAnimation.h"
#include <unordered_set>
#include <string>
#include <filesystem>

namespace wallpaper
{
namespace fs
{
class VFS;
}

namespace wpscene
{

class WPEffectCommand {
public:
    bool        FromJson(const nlohmann::json&);
    std::string command;
    std::string target;
    std::string source;

    i32 afterpos { 0 }; // 0 for begin, start from 1
};

class WPEffectFbo {
public:
    bool        FromJson(const nlohmann::json&);
    std::string name;
    std::string format;
    uint32_t    scale { 1 };
};

class WPImageEffect {
private:
    static const std::unordered_set<std::string> BLACKLISTED_WORKSHOP_EFFECTS;
    bool                                         IsEffectBlacklisted(const std::string& filePath);

public:
    bool                         FromJson(const nlohmann::json&, fs::VFS& vfs);
    bool                         FromFileJson(const nlohmann::json&, fs::VFS& vfs);
    int32_t                      id;
    std::string                  name;
    bool                         visible { true };
    int32_t                      version;
    std::vector<WPMaterial>      materials;
    std::vector<WPMaterialPass>  passes;
    std::vector<WPEffectCommand> commands;
    std::vector<WPEffectFbo>     fbos;
};

class WPImageObject {
public:
    struct Config {
        bool passthrough { false };
    };
    bool                 FromJson(const nlohmann::json&, fs::VFS&);
    int32_t              id { 0 };
    int32_t              parent_id { -1 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> size { 2.0f, 2.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    int32_t              colorBlendMode { 0 };
    float                alpha { 1.0f };
    float                brightness { 1.0f };
    bool                 fullscreen { false };
    bool                 nopadding { false };
    // Set when the image-model JSON declares "autosize": true.  Resolved in
    // ParseImageObj by reading the first texture's sprite-frame dimensions
    // (or mapWidth × mapHeight for non-sprite textures).
    bool autosize { false };
    // Model JSON "solidlayer": true — placeholder layer with no visible base.
    // WE's flat shader convention is that solidlayers render fully transparent
    // (g_Alpha = 0) so the effect chain reads (0,0,0,0) for the first pass and
    // builds visible content via effects only.  Without this override, our
    // base pass would render an opaque white quad if g_Color/g_Alpha were
    // populated, polluting any per-image effect chain.
    bool solidlayer { false };
    // Scene-level "copybackground": true (default) means a passthrough compose
    // layer captures _rt_default into its pingpong before running its effect
    // chain.  When false, the effect chain runs on whatever the scene graph
    // has already written into the pingpong (e.g. children rendered there) —
    // suppress the implicit screen copy in SceneToRenderGraph's passthrough
    // path so the pingpong isn't overwritten.  Default true matches WE
    // semantics for composelayers.
    bool copybackground { true };
    bool visible { true };
    bool visibleIsComboSelector { false }; // combo condition-based visibility (skip offscreen)
    // WE editor flag: when true, this object doesn't inherit transform /
    // visibility / alpha / tint from parent groups or nodes.  Parsed for
    // completeness; currently no group hierarchy exists in the scenes we
    // handle, so nothing to propagate.
    bool disablepropagation { false };
    bool perspective { false };            // Use perspective camera (default: flat/ortho)
    std::string                image;
    std::string                alignment { "center" };
    WPMaterial                 material;
    std::vector<WPImageEffect> effects;
    Config                     config;

    std::string                                puppet;
    std::vector<WPPuppetLayer::AnimationLayer> puppet_layers;

    // Parent-puppet attachment point name (scene.json "attachment" field).
    // When non-empty and the parent is a puppet, this child anchors to the
    // named MDAT attachment in the parent puppet's skeleton instead of the
    // parent mesh center.  e.g. hair pieces with attachment="head" end up
    // at the head anchor in asuna body's skeleton.
    std::string attachment;

    // Keyframe animations parsed out of <prop>.animation blocks.
    // Each entry targets a specific scalar property on this object (e.g. alpha).
    std::vector<PropertyAnimation> propertyAnimations;

    // Color property script (e.g. audio-reactive color)
    std::string colorScript;
    std::string colorScriptProperties; // JSON string
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPEffectFbo, name, scale);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPImageEffect, name, visible, passes, fbos, materials);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPImageObject, name, origin, angles, scale, size, visible,
                                   material, effects);

} // namespace wpscene
} // namespace wallpaper
