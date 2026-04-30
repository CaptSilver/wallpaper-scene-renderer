#pragma once
#include <array>
#include <memory>
#include <string_view>
#include <string>
#include <vector>
#include <functional>
#include "Type.hpp"
#include "WPVolumeAnimation.h"
#include "Swapchain/ExSwapchain.hpp"

namespace wallpaper::audio
{
class AudioAnalyzer;
}

namespace wallpaper
{

using FirstFrameCallback = std::function<void()>;

struct TextScriptInfo {
    int32_t     id;
    std::string script;
    std::string scriptProperties; // JSON string
    std::string initialValue;
};

struct ColorScriptInfo {
    int32_t              id;
    std::string          script;
    std::string          scriptProperties;
    std::array<float, 3> initialColor;
};

struct PropertyScriptInfo {
    // Where the script is attached.  Object-attached scripts bind thisObject
    // to thisLayer; AnimationLayer-attached scripts bind thisObject to the
    // specific rig layer (Lucy puppet offset scripts rely on this).
    enum class Attachment : uint8_t
    {
        Object         = 0,
        AnimationLayer = 1,
    };

    int32_t              id;
    std::string          property; // "visible", "origin", "scale", "angles", "alpha"
    std::string          script;
    std::string          scriptProperties; // JSON string
    std::string          layerName;        // name of the parent object for thisLayer.name
    bool                 initialVisible { true };
    std::array<float, 3> initialVec3 { 0, 0, 0 };
    float                initialFloat { 1.0f };
    Attachment           attachment { Attachment::Object };
    int32_t              animationLayerIndex { -1 };
};

struct VolumeAnimInfo {
    std::string                                name;
    std::string                                mode { "loop" };
    float                                      fps { 30.0f };
    float                                      length { 0 };
    std::vector<wallpaper::VolumeAnimKeyframe> keyframes;
};
struct SoundVolumeScriptInfo {
    int32_t        index;
    std::string    script;
    std::string    scriptProperties;
    std::string    layerName;
    float          initialVolume { 1.0f };
    bool           hasAnimation { false };
    VolumeAnimInfo animation;
};

struct SoundLayerControlInfo {
    std::string name;
    float       initialVolume { 1.0f };
    bool        startsilent { false };
};

// Puppet animation keyframe event fired by the render thread and drained by
// the QML/script thread. Routed to the owning object's SceneScript handler
// (animationEvent(event, value)).
struct AnimationEventInfo {
    int32_t     nodeId { -1 };
    int32_t     frame { 0 };
    std::string name;
};

constexpr std::string_view PROPERTY_SOURCE               = "source";
constexpr std::string_view PROPERTY_ASSETS               = "assets";
constexpr std::string_view PROPERTY_FPS                  = "fps";
constexpr std::string_view PROPERTY_FILLMODE             = "fillmode";
constexpr std::string_view PROPERTY_SPEED                = "speed";
constexpr std::string_view PROPERTY_GRAPHIVZ             = "graphivz";
constexpr std::string_view PROPERTY_VOLUME               = "volume";
constexpr std::string_view PROPERTY_MUTED                = "muted";
constexpr std::string_view PROPERTY_CACHE_PATH           = "cache_path";
constexpr std::string_view PROPERTY_FIRST_FRAME_CALLBACK = "first_frame_callback";
constexpr std::string_view PROPERTY_USER_PROPS           = "user_props";
constexpr std::string_view PROPERTY_HDR_OUTPUT           = "hdr_output";
constexpr std::string_view PROPERTY_HDR_CONTENT          = "hdr_content";
constexpr std::string_view PROPERTY_SYSTEM_AUDIO_CAPTURE = "system_audio_capture";
constexpr std::string_view PROPERTY_SCREENSHOT_PATH      = "screenshot_path";

#include "Core/NoCopyMove.hpp"
class MainHandler;
struct RenderInitInfo;

class SceneWallpaper : NoCopy {
public:
    SceneWallpaper();
    ~SceneWallpaper();
    bool init();
    bool inited() const;

    void initVulkan(const RenderInitInfo&);

    void play();
    void pause();
    void mouseInput(double x, double y);
    void updateText(int32_t id, const std::string& text);
    // Dynamic pointsize override: scripts call `thisLayer.pointsize = 20`
    // when fitting long track titles.  Next text update will rasterize at
    // the new pointsize.  <=0 means "keep whatever was authored".
    void updateTextPointsize(int32_t id, float pointsize);
    // Debug per-pass RT dump: writes every CustomShaderPass output image
    // after the next frame completes.  See VulkanRender::setPassDumpDir.
    void                                     requestPassDump(const std::string& dir);
    bool                                     passDumpDone() const;
    void                                     updateColor(int32_t id, float r, float g, float b);
    std::vector<TextScriptInfo>              getTextScripts() const;
    std::vector<ColorScriptInfo>             getColorScripts() const;
    std::vector<PropertyScriptInfo>          getPropertyScripts() const;
    std::unordered_map<std::string, int32_t> getNodeNameToIdMap() const;
    std::string                              getLayerInitialStatesJson() const;
    std::array<int32_t, 2>                   getOrthoSize() const;
    // Camera-parallax config (enable / amount / mouse-influence) + camera
    // position in scene units.  Exposed so cursor hit-testing can mirror the
    // MVP parallax offset WPShaderValueUpdater applies to rendered quads.
    struct ParallaxInfo {
        bool  enable { false };
        float amount { 0.5f };
        float mouseInfluence { 0.1f };
        float camX { 0.0f };
        float camY { 0.0f };
    };
    ParallaxInfo getParallaxInfo() const;
    void updateNodeTransform(int32_t id, const std::string& property, float x, float y, float z);
    void updateNodeVisible(int32_t id, bool visible);
    void updateNodeAlpha(int32_t id, float alpha);

    // Scripted particle instance-override dispatch.  Currently only `rate` is
    // implemented; the script returns a multiplier that scales emission speed
    // on top of the static instanceoverride.rate value captured at parse time.
    // No-op on IDs that don't own a particle subsystem.
    void updateParticleRate(int32_t id, float rate);

    // Batched per-tick layer update — one lock for the entire dispatch pass.
    // Flags: 1=origin 2=scale 4=angles 8=visible 16=alpha (matches JS
    // DIRTY_STRIDE F_* constants).  Unset fields are ignored.
    struct LayerBatchUpdate {
        int32_t  id;
        uint32_t flags;
        float    origin[3];
        float    scale[3];
        float    angles[3];
        float    alpha;
        uint8_t  visible;
    };
    void applyLayerBatch(const std::vector<LayerBatchUpdate>& batch);

    void updateEffectVisible(int32_t nodeId, int32_t effectIndex, bool visible);
    // IMaterial.setValue from SceneScript — enqueues for the render thread,
    // applied to mesh.Material()->customShader.constValues with dirty flag.
    void updateMaterialValue(int32_t            nodeId,
                             std::string        name,
                             std::vector<float> floats);

    // Layer-hierarchy bridge — thisLayer.setParent() JS path enqueues
    // a (childId, parentId) pair into Scene::m_pending_parent_changes,
    // which is drained at the start of RenderHandler::CMD_DRAW.  parentId
    // == -1 means "reattach to scene root".
    void queueParentChange(int32_t childId, int32_t parentId);

    // SceneScript thisScene.sortLayer(layer, index) bridge.  Enqueues a
    // (childId, targetIndex) pair into Scene::m_pending_child_sorts, drained
    // alongside the parent-change queue at the start of CMD_DRAW.
    // targetIndex clamps to [0, parent->children.size()-1].
    void queueChildSort(int32_t childId, int32_t targetIndex);

    std::vector<SoundVolumeScriptInfo> getSoundVolumeScripts() const;
    void                               updateSoundVolume(int32_t index, float volume);
    std::string                        getUserPropertiesJson() const;
    // Render-thread scene clock (seconds since scene load). Same clock as visuals:
    // advances only when drawFrame() runs, so audio animations stay locked to render
    // cadence instead of wall-clock.
    double getSceneTime() const;

    // Video texture control API for SceneScript thisLayer.getVideoTexture().
    // All operations take the nodeId of the layer owning the video texture.
    // Calls on unknown IDs are safe no-ops / return 0.
    std::vector<int32_t> getVideoTextureNodeIds() const;
    double               videoGetCurrentTime(int32_t nodeId) const;
    double               videoGetDuration(int32_t nodeId) const;
    bool                 videoIsPlaying(int32_t nodeId) const;
    void                 videoPlay(int32_t nodeId);
    void                 videoPause(int32_t nodeId);
    void                 videoStop(int32_t nodeId);
    void                 videoSetCurrentTime(int32_t nodeId, double t);
    void                 videoSetRate(int32_t nodeId, double rate);

    // Sound layer control API for SceneScript play/stop/pause
    std::vector<SoundLayerControlInfo> getSoundLayerControls() const;
    void                               soundLayerPlay(int32_t index);
    void                               soundLayerStop(int32_t index);
    void                               soundLayerPause(int32_t index);
    bool                               soundLayerIsPlaying(int32_t index) const;
    void                               soundLayerSetVolume(int32_t index, float volume);

    // Drain puppet-animation keyframe events that fired since the last call.
    // Polled by SceneBackend each script evaluation tick.
    std::vector<AnimationEventInfo> drainAnimationEvents();

    // Named property-animation control (layer.getAnimation(name).play()/etc).
    void propertyAnimPlay(int32_t nodeId, const std::string& name);
    void propertyAnimPause(int32_t nodeId, const std::string& name);
    void propertyAnimStop(int32_t nodeId, const std::string& name);
    bool propertyAnimIsPlaying(int32_t nodeId, const std::string& name) const;

    // Scene property control (bloom, clear color, camera, lighting)
    void updateClearColor(float r, float g, float b);
    void updateBloomStrength(float strength);
    void updateBloomThreshold(float threshold);
    void updateCameraFov(float fov);
    void updateCameraLookAt(float ex, float ey, float ez, float cx, float cy, float cz, float ux,
                            float uy, float uz);
    void updateAmbientColor(float r, float g, float b);
    void updateSkylightColor(float r, float g, float b);
    void updateLightColor(int32_t index, float r, float g, float b);
    void updateLightRadius(int32_t index, float radius);
    void updateLightIntensity(int32_t index, float intensity);
    void updateLightPosition(int32_t index, float x, float y, float z);
    std::string getSceneInitialStateJson() const;

    void setPropertyBool(std::string_view, bool);
    void setPropertyInt32(std::string_view, int32_t);
    void setPropertyFloat(std::string_view, float);
    void setPropertyString(std::string_view, std::string);
    void setPropertyObject(std::string_view, std::shared_ptr<void>);

    ExSwapchain*                          exSwapchain() const;
    std::shared_ptr<audio::AudioAnalyzer> audioAnalyzer() const;

    // Screenshot capture: request a PPM dump of the next presented swapchain
    // image.  screenshotDone() flips true once the file is written.
    void requestScreenshot(const std::string& path);
    bool screenshotDone() const;

    // Debug: hide any scene object whose name contains any comma-separated
    // needle.  Must be set before scene load.  Empty disables.
    void setHidePattern(const std::string& pat);

private:
    bool m_inited { false };

private:
    friend class MainHandler;

    bool                         m_offscreen { false };
    std::shared_ptr<MainHandler> m_main_handler;
};
} // namespace wallpaper
