#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"

#include "Utils/Logging.h"
#include "Utils/SceneProfiler.h"
#include "Looper/Looper.hpp"

#include "Timer/FrameTimer.hpp"
#include "Utils/FpsCounter.h"
#include "WPSceneParser.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneImageEffectLayer.h"
#include "Scene/SubTickPlan.hpp"
#include "Scene/WorldCacheGate.h"
#include "Scene/TextStyleMerge.hpp"
#include "Particle/ParticleSystem.h"
#include "Particle/AudioRateMultiplier.hpp"
#include "Core/Random.hpp"
#include "Interface/IShaderValueUpdater.h"
#include "WPShaderValueUpdater.hpp"

#include "Fs/VFS.h"
#include "Fs/PhysicalFs.h"
#include "WPPkgFs.hpp"

#include "WPSoundParser.hpp"
#include "Audio/SoundManager.h"
#include "Audio/AudioAnalyzer.h"
#include "Audio/AudioCapture.h"
#include "VideoTextureDecoder.hpp"
#ifdef HAVE_EGL_HWDEC
#    include "HWVideoTextureDecoder.hpp"
#endif
#include "Image.hpp"

#include "RenderGraph/RenderGraph.hpp"

#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/VolumetricChain.hpp"
#include "VulkanRender/VulkanRender.hpp"
#include "WPTextRenderer.hpp"
#include "WPPuppet.hpp" // for getLayerBoneIndex attachment lookup

#include "WPUserProperties.hpp"
#include "WPSceneFileResolver.hpp"
#include <nlohmann/json.hpp>
#include <sstream>
#include <atomic>
#include <mutex>
#include <optional>
#include <set>
#include <map>
#include <fstream>
#include <filesystem>
#include <unordered_map>

using namespace wallpaper;

#define CASE_CMD(cmd) \
    case CMD::CMD_##cmd: handle_##cmd(msg); break;
#define MHANDLER_CMD(cmd) void handle_##cmd(const std::shared_ptr<looper::Message>& msg)
#define MHANDLER_CMD_IMPL(cl, cmd) \
    void impl_##cl::handle_##cmd(const std::shared_ptr<looper::Message>& msg)
#define CALL_MHANDLER_CMD(cmd, msg) handle_##cmd(msg)

namespace
{
template<typename T>
void AddMsgCmd(looper::Message& msg, T cmd) {
    msg.setInt32("cmd", (int32_t)cmd);
}
template<typename T>
std::shared_ptr<looper::Message> CreateMsgWithCmd(const std::shared_ptr<looper::Handler>& handler,
                                                  T                                       cmd) {
    auto msg = looper::Message::create(0, handler);
    AddMsgCmd(*msg, cmd);
    return msg;
}
} // namespace

namespace wallpaper
{
class RenderHandler;

class MainHandler : public looper::Handler {
public:
    enum class CMD
    {
        CMD_LOAD_SCENE,
        CMD_SET_PROPERTY,
        CMD_STOP,
        CMD_FIRST_FRAME,
        CMD_NO
    };

public:
    MainHandler();
    virtual ~MainHandler() {};

    void setHidePattern(const std::string& pat) { m_scene_parser.SetHidePattern(pat); }

    bool init();
    auto renderHandler() const { return m_render_handler; }
    bool inited() const { return m_inited; }

public:
    void onMessageReceived(const std::shared_ptr<looper::Message>& msg) override {
        int32_t cmd_int = (int32_t)CMD::CMD_NO;
        if (msg->findInt32("cmd", &cmd_int)) {
            CMD cmd = static_cast<CMD>(cmd_int);
            switch (cmd) {
                CASE_CMD(SET_PROPERTY);
                CASE_CMD(LOAD_SCENE);
                CASE_CMD(STOP);
                CASE_CMD(FIRST_FRAME);
            default: break;
            }
        }
    }

    void sendCmdLoadScene();
    void sendFirstFrameOk();
    bool isGenGraphviz() const { return m_gen_graphviz; }

    std::vector<TextScriptInfo> getTextScripts() const {
        std::lock_guard<std::mutex> lock(m_text_scripts_mutex);
        return m_text_scripts;
    }

    std::vector<ColorScriptInfo> getColorScripts() const {
        std::lock_guard<std::mutex> lock(m_color_scripts_mutex);
        return m_color_scripts;
    }

    std::vector<ShaderValueScriptInfo> getShaderValueScripts() const {
        std::lock_guard<std::mutex> lock(m_shader_value_scripts_mutex);
        return m_shader_value_scripts;
    }

    std::vector<PropertyScriptInfo> getPropertyScripts() const {
        std::lock_guard<std::mutex> lock(m_property_scripts_mutex);
        return m_property_scripts;
    }

    std::vector<SoundVolumeScriptInfo> getSoundVolumeScripts() const {
        std::lock_guard<std::mutex> lock(m_sound_volume_scripts_mutex);
        return m_sound_volume_scripts;
    }

    std::string getUserPropertiesJson() const {
        std::lock_guard<std::mutex> lock(m_user_props_mutex);
        return m_user_props_resolved.ToJson();
    }

    void updateSoundVolume(int32_t index, float volume) {
        std::lock_guard<std::mutex> lock(m_sound_volume_scripts_mutex);
        if (index >= 0 && index < (int32_t)m_sound_volume_streams.size()) {
            WPSoundParser::SetStreamVolume(m_sound_volume_streams[index], volume);
        }
    }

    std::vector<SoundLayerControlInfo> getSoundLayerControls() const {
        std::lock_guard<std::mutex> lock(m_sound_layers_mutex);
        return m_sound_layer_controls;
    }
    void soundLayerPlay(int32_t index) {
        std::lock_guard<std::mutex> lock(m_sound_layers_mutex);
        if (index >= 0 && index < (int32_t)m_sound_layer_streams.size())
            WPSoundParser::StreamPlay(m_sound_layer_streams[index]);
    }
    void soundLayerStop(int32_t index) {
        std::lock_guard<std::mutex> lock(m_sound_layers_mutex);
        if (index >= 0 && index < (int32_t)m_sound_layer_streams.size())
            WPSoundParser::StreamStop(m_sound_layer_streams[index]);
    }
    void soundLayerPause(int32_t index) {
        std::lock_guard<std::mutex> lock(m_sound_layers_mutex);
        if (index >= 0 && index < (int32_t)m_sound_layer_streams.size())
            WPSoundParser::StreamPause(m_sound_layer_streams[index]);
    }
    bool soundLayerIsPlaying(int32_t index) const {
        std::lock_guard<std::mutex> lock(m_sound_layers_mutex);
        if (index >= 0 && index < (int32_t)m_sound_layer_streams.size())
            return WPSoundParser::StreamIsPlaying(m_sound_layer_streams[index]);
        return false;
    }
    void soundLayerSetVolume(int32_t index, float volume) {
        std::lock_guard<std::mutex> lock(m_sound_layers_mutex);
        if (index >= 0 && index < (int32_t)m_sound_layer_streams.size())
            WPSoundParser::SetStreamVolume(m_sound_layer_streams[index], volume);
    }

    std::unordered_map<std::string, int32_t> getNodeNameToIdMap() const {
        std::lock_guard<std::mutex> lock(m_name_map_mutex);
        return m_node_name_to_id;
    }

    std::string getLayerInitialStatesJson() const {
        std::lock_guard<std::mutex> lock(m_layer_init_mutex);
        return m_layer_init_json;
    }

    std::string getSceneInitialStateJson() const {
        std::lock_guard<std::mutex> lock(m_scene_init_mutex);
        return m_scene_init_json;
    }

    std::array<int32_t, 2> getOrthoSize() const {
        auto scene = m_scene.load();
        if (scene) return { scene->ortho[0], scene->ortho[1] };
        return { 1920, 1080 };
    }

    SceneWallpaper::ParallaxInfo getParallaxInfo() const {
        SceneWallpaper::ParallaxInfo info;
        auto                         scene = m_scene.load();
        if (! scene) return info;
        auto* updater = dynamic_cast<WPShaderValueUpdater*>(scene->shaderValueUpdater.get());
        if (updater) {
            const auto& p       = updater->GetCameraParallax();
            info.enable         = p.enable;
            info.amount         = p.amount;
            info.mouseInfluence = p.mouseinfluence;
        }
        if (scene->activeCamera) {
            auto pos  = scene->activeCamera->GetPosition();
            info.camX = (float)pos.x();
            info.camY = (float)pos.y();
        }
        return info;
    }

    std::shared_ptr<audio::AudioAnalyzer> audioAnalyzer() const { return m_audio_analyzer; }

    // push the SceneScript audio-consumer term (engine._audioRegs
    // non-empty) into the current scene's updater so the FFT gate keeps
    // Process() running for script-only-audio scenes (the CRITICAL starvation
    // guard).  Called from the QML thread (SceneObject::setupTextScripts, fired
    // on firstFrame, so the scene is already published); .load() stabilises the
    // pointer and the updater flag is atomic.
    void setHasScriptAudio(bool v) {
        auto scene = m_scene.load();
        if (! scene) return;
        if (auto* updater = dynamic_cast<WPShaderValueUpdater*>(scene->shaderValueUpdater.get()))
            updater->SetHasScriptAudio(v);
    }

    std::vector<AnimationEventInfo> drainAnimationEvents() {
        // m_scene is an atomic<shared_ptr> re-published by loadScene on the main
        // looper thread (a reload re-assigns it), and read here on the QML
        // thread.  .load() takes a stable local so the pointer can't tear under
        // a concurrent reload; the underlying DrainAnimationEvents() is itself
        // mutex-protected against the render-thread writer.
        auto scene = m_scene.load();
        if (! scene) return {};
        auto* updater = dynamic_cast<WPShaderValueUpdater*>(scene->shaderValueUpdater.get());
        if (! updater) return {};
        auto                            raw = updater->DrainAnimationEvents();
        std::vector<AnimationEventInfo> out;
        out.reserve(raw.size());
        for (auto& e : raw) {
            out.push_back({ e.nodeId, e.frame, std::move(e.name) });
        }
        return out;
    }

private:
    void loadScene();
    bool applyUserPropsRuntime(const std::string& newJson);

    MHANDLER_CMD(LOAD_SCENE);
    MHANDLER_CMD(SET_PROPERTY);
    MHANDLER_CMD(STOP);
    MHANDLER_CMD(FIRST_FRAME);

private:
    bool m_inited { false };

    std::string m_assets;
    std::string m_source;
    std::string m_cache_path;
    bool        m_gen_graphviz { false };
    bool        m_hdr_output { false };
    bool        m_system_audio_capture { false };
    std::string m_postprocessing_override;

    WPSceneParser                         m_scene_parser;
    std::unique_ptr<audio::SoundManager>  m_sound_manager;
    std::shared_ptr<audio::AudioAnalyzer> m_audio_analyzer;
    std::unique_ptr<audio::AudioCapture>  m_audio_capture;
    FirstFrameCallback                    m_first_frame_callback;
    std::string                           m_user_props_json;
    // Atomic: written on the main looper thread (loadScene) and read on the
    // QML thread (getOrthoSize / getParallaxInfo / drainAnimationEvents).  Every
    // access goes through .load()/.store() so a reload's re-publish can't tear
    // the pointer under a concurrent reader.  Shared with the render handler.
    std::atomic<std::shared_ptr<Scene>> m_scene;

    mutable std::mutex                       m_text_scripts_mutex;
    std::vector<TextScriptInfo>              m_text_scripts;
    mutable std::mutex                       m_color_scripts_mutex;
    std::vector<ColorScriptInfo>             m_color_scripts;
    mutable std::mutex                       m_shader_value_scripts_mutex;
    std::vector<ShaderValueScriptInfo>       m_shader_value_scripts;
    mutable std::mutex                       m_property_scripts_mutex;
    std::vector<PropertyScriptInfo>          m_property_scripts;
    mutable std::mutex                       m_sound_volume_scripts_mutex;
    std::vector<SoundVolumeScriptInfo>       m_sound_volume_scripts;
    std::vector<void*>                       m_sound_volume_streams; // parallel: WPSoundStream*
    mutable std::mutex                       m_sound_layers_mutex;
    std::vector<SoundLayerControlInfo>       m_sound_layer_controls;
    std::vector<void*>                       m_sound_layer_streams; // parallel: WPSoundStream*
    mutable std::mutex                       m_name_map_mutex;
    std::unordered_map<std::string, int32_t> m_node_name_to_id;
    mutable std::mutex                       m_layer_init_mutex;
    std::string                              m_layer_init_json;
    mutable std::mutex                       m_scene_init_mutex;
    std::string                              m_scene_init_json;
    mutable std::mutex                       m_user_props_mutex;
    WPUserProperties                         m_user_props_resolved; // for runtime re-resolution

private:
    std::shared_ptr<looper::Looper> m_main_loop;
    std::shared_ptr<looper::Looper> m_render_loop;
    std::shared_ptr<RenderHandler>  m_render_handler;
};
// for macro
using impl_MainHandler = MainHandler;

class RenderHandler : public looper::Handler {
public:
    enum class CMD
    {
        CMD_INIT_VULKAN,
        CMD_SET_SCENE,
        CMD_SET_FILLMODE,
        CMD_SET_SPEED,
        CMD_SET_HDR,
        CMD_STOP,
        CMD_DRAW,
        CMD_NO
    };
    MainHandler& main_handler;
    RenderHandler(MainHandler& m)
        : main_handler(m), m_render(std::make_unique<vulkan::VulkanRender>()) {}
    virtual ~RenderHandler() {
        frame_timer.Stop();
        // Release Scene GPU resources before destroying VMA allocator
        auto scene = m_scene.load();
        if (scene && m_render->inited()) {
            m_render->clearLastRenderGraph(scene.get());
        }
        m_render->destroy();
        LOG_INFO("render handler deleted");
    }

    void onMessageReceived(const std::shared_ptr<looper::Message>& msg) override {
        int32_t cmd_int = (int32_t)CMD::CMD_NO;
        if (msg->findInt32("cmd", &cmd_int)) {
            CMD cmd = static_cast<CMD>(cmd_int);
            switch (cmd) {
                CASE_CMD(DRAW);
                CASE_CMD(STOP);
                CASE_CMD(SET_FILLMODE);
                CASE_CMD(SET_SCENE);
                CASE_CMD(SET_SPEED);
                CASE_CMD(SET_HDR);
                CASE_CMD(INIT_VULKAN);
            default: break;
            }
        }
    }

    ExSwapchain* exSwapchain() const { return m_render->exSwapchain(); }

    bool renderInited() const { return m_render->inited(); }

    void requestScreenshot(const std::string& path) {
        if (m_render) m_render->setScreenshotPath(path);
    }
    bool screenshotDone() const { return m_render && m_render->screenshotDone(); }

    // Arm a screenshot to fire on the render thread exactly when the monotonic
    // frame counter reaches `frame`.  Unlike requestScreenshot() (which captures
    // the next frame after an async path-set, racing the wall clock), this is
    // frame-deterministic: identical target frame → identical captured
    // scene-frame, so a cold run and a warm run produce byte-identical output.
    void requestScreenshotAtFrame(const std::string& path, uint64_t frame) {
        {
            std::lock_guard<std::mutex> lk(m_shot_at_mutex);
            m_shot_at_path = path;
        }
        m_shot_at_frame.store(static_cast<int64_t>(frame), std::memory_order_release);
    }

    void requestPassDump(const std::string& dir) {
        if (m_render) m_render->setPassDumpDir(dir);
    }
    bool passDumpDone() const { return m_render && m_render->passDumpDone(); }

    void setMousePos(double x, double y) { m_mouse_pos.store(std::array { (float)x, (float)y }); }

    void setTextUpdate(i32 id, const std::string& text) {
        std::lock_guard<std::mutex> lock(m_text_update_mutex);
        m_pending_text_updates[id] = text;
    }

    void setTextPointsize(i32 id, float pointsize) {
        std::lock_guard<std::mutex> lock(m_text_update_mutex);
        m_pending_pointsize_updates[id] = pointsize;
    }

    // Merge-style queue: subsequent calls for the same id within one tick
    // overwrite earlier values per-field (empty string = "leave unchanged").
    // SceneScript users typically write each property independently
    // (`thisLayer.horizontalalign = 'right'; thisLayer.font = 'Heavy.otf';`),
    // so each setter posts only the field it touches.
    void setTextStyle(i32 id, std::string halign, std::string valign,
                      std::string fontName) {
        std::lock_guard<std::mutex> lock(m_text_update_mutex);
        mergeTextStyle(m_pending_text_style_updates[id], halign, valign, fontName);
    }

    // World-matrix cache populated at end of each drawFrame.  SceneScript
    // thisLayer.getTransformMatrix() reads from here via SceneWallpaper.
    // Returns identity if the id is unknown (e.g. layer not yet visible /
    // node destroyed); identity is a safe fallback because scripts mainly
    // inspect translation indices [12,13] which read as 0 from identity.
    std::array<float, 16> getLayerWorldMatrix(i32 id) const {
        // a reader has appeared: enable the per-frame rebuild from now
        // on.  Idempotent; relaxed.  The very first read (before the producer
        // observes the flag) falls through to the identity fallback, which is
        // the documented not-yet-cached behavior.
        markWorldCacheNeeded(m_needs_world_cache);
        std::lock_guard<std::mutex> lk(m_world_cache_mutex);
        auto it = m_layer_world_cache.find(id);
        if (it != m_layer_world_cache.end()) return it->second;
        return worldCacheIdentity();
    }

    // SceneScript thisLayer.getTextureAnimation() read-back — returns the
    // per-node sprite playback snapshot published each render tick by
    // WPShaderValueUpdater after the sprite advances.  All-zero / not-pinned
    // when the node has no sprite or hasn't been drawn yet, which the JS
    // proxy maps to `{frameCount: 0/1, currentFrame: 0, duration: 0,
    // isPlaying: true}`.  Held under nodeSpriteSnapshotMutex so the
    // render-thread writer and JS-thread reader don't race.
    Scene::NodeSpriteSnapshot getLayerSpriteSnapshot(i32 nodeId) const {
        auto scene = m_scene.load();
        if (! scene) return {};
        std::lock_guard<std::mutex> lk(scene->nodeSpriteSnapshotMutex);
        auto                        it = scene->nodeSpriteSnapshot.find(nodeId);
        if (it == scene->nodeSpriteSnapshot.end()) return {};
        return it->second;
    }

    // SceneScript thisLayer.getBoneIndex(name) — resolves a named MDAT
    // attachment in the puppet rigged to this layer (or its parent's puppet
    // for child-rigged layers).  nodePuppetMap is populated at parse time and
    // never mutated thereafter (WPPuppet attachments are likewise immutable),
    // so the puppet lookups are content-safe; the surrounding m_scene pointer
    // is .load()ed once into a local so a concurrent render-thread reset can't
    // tear it.  The parent walk (for child-rigged layers) reads SceneNode
    // parent pointers that the render thread reparents via the drain, so it
    // goes through Scene::ResolveParentNodeId under the Scene parent-tree lock
    // (B5b) rather than touching nodeById/Parent() inline.  Returns 0
    // ("origin") if not found — matches WE's missing-attachment sentinel.
    i32 getLayerBoneIndex(i32 nodeId, const std::string& boneName) const {
        auto scene = m_scene.load();
        if (! scene || boneName.empty()) return 0;
        auto try_lookup = [&](i32 id) -> i32 {
            if (id < 0) return -1;
            auto it = scene->nodePuppetMap.find(id);
            if (it == scene->nodePuppetMap.end() || ! it->second) return -1;
            auto* att = it->second->findAttachment(boneName);
            if (! att) return -1;
            return static_cast<i32>(att->bone_index);
        };
        i32 r = try_lookup(nodeId);
        if (r >= 0) return r;
        r = try_lookup(scene->ResolveParentNodeId(nodeId));
        if (r >= 0) return r;
        return 0;
    }

    void setColorUpdate(i32 id, float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_color_update_mutex);
        m_pending_color_updates[id] = { r, g, b };
        LOG_INFO("setColorUpdate enqueued id=%d rgb=(%.3f,%.3f,%.3f)", id, r, g, b);
    }

    void setNodeTransform(i32 id, const std::string& property, float x, float y, float z) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        auto                        key  = std::make_pair(id, property);
        m_pending_transform_updates[key] = { x, y, z };
        static int s_transform_log       = 0;
        // Keep a long-lived "last logged" map so [jump] reflects real deltas;
        // the pending map clears every frame, which would otherwise mark
        // every first-of-frame write as a jump.
        static std::map<std::pair<i32, std::string>, std::array<float, 3>> s_last_logged_transform;
        auto prev   = s_last_logged_transform.find(key);
        bool jumped = (prev == s_last_logged_transform.end()) ||
                      (std::abs(prev->second[0] - x) + std::abs(prev->second[1] - y) +
                       std::abs(prev->second[2] - z)) > 50.0f;
        if (++s_transform_log <= 5 || jumped) {
            LOG_INFO("setNodeTransform[%d]: id=%d prop=%s val=(%.4f,%.4f,%.4f)%s",
                     s_transform_log,
                     id,
                     property.c_str(),
                     x,
                     y,
                     z,
                     jumped ? " [jump]" : "");
            s_last_logged_transform[key] = { x, y, z };
        }
    }

    void setNodeVisible(i32 id, bool visible) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_visible_updates[id]                      = visible;
        static int                           s_visible_log = 0;
        static std::unordered_map<i32, bool> s_last_logged_visible;
        auto                                 prev = s_last_logged_visible.find(id);
        bool jumped = prev == s_last_logged_visible.end() || prev->second != visible;
        if (++s_visible_log <= 5 || jumped) {
            LOG_INFO("setNodeVisible[%d]: id=%d visible=%d%s",
                     s_visible_log,
                     id,
                     (int)visible,
                     jumped ? " [jump]" : "");
            s_last_logged_visible[id] = visible;
        }
    }

    void setEffectVisible(i32 nodeId, i32 effectIndex, bool visible) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_effect_visible.emplace_back(nodeId, effectIndex, visible);
    }

    void setMaterialValue(i32 nodeId, std::string name, std::vector<float> floats) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_material_values.emplace_back(nodeId, std::move(name), std::move(floats));
    }

    void setEffectMaterialValue(i32 nodeId, i32 effectIdx, std::string name,
                                std::vector<float> floats) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_effect_material_values.emplace_back(
            nodeId, effectIdx, std::move(name), std::move(floats));
    }

    void setLayerSpriteFrame(i32 nodeId, bool wantsManual, i32 frameIdx) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_sprite_frame[nodeId] = { wantsManual, frameIdx };
    }

    void queueParentChange(i32 childId, i32 parentId) {
        // Called inline on the QML thread; .load() the scene pointer so it
        // can't tear under a render-thread reset.  Scene owns its own mutex on
        // the queue, so no extra locking on the enqueue itself.
        auto scene = m_scene.load();
        if (scene) scene->QueueParentChange(childId, parentId);
    }

    void queueChildSort(i32 childId, i32 targetIndex) {
        auto scene = m_scene.load();
        if (scene) scene->QueueChildSort(childId, targetIndex);
    }

    void setNodeAlpha(i32 id, float alpha) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_alpha_updates[id] = alpha;
        static int s_alpha_log      = 0;
        // Track the last *logged* value separately from the pending map
        // (which gets cleared each render frame) so [jump] detects real
        // deltas instead of firing on every first-of-frame write.
        static std::unordered_map<i32, float> s_last_logged_alpha;
        auto                                  prev = s_last_logged_alpha.find(id);
        bool jumped = prev == s_last_logged_alpha.end() || std::abs(prev->second - alpha) > 0.3f;
        if (++s_alpha_log <= 5 || jumped) {
            LOG_INFO("setNodeAlpha[%d]: id=%d alpha=%.4f%s",
                     s_alpha_log,
                     id,
                     alpha,
                     jumped ? " [jump]" : "");
            s_last_logged_alpha[id] = alpha;
        }
    }

    void setParticleRate(i32 id, float rate) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_particle_rate[id]                      = rate;
        static int                            s_rate_log = 0;
        static std::unordered_map<i32, float> s_last_logged_rate;
        auto                                  prev = s_last_logged_rate.find(id);
        // Log the first few writes + any subsequent >30% swing so NieR 2B's
        // bass-driven 0.1..1.0 oscillation is visible without flooding the
        // journal at every tick.
        bool jumped = prev == s_last_logged_rate.end() || std::abs(prev->second - rate) > 0.3f;
        if (++s_rate_log <= 5 || jumped) {
            LOG_INFO("setParticleRate[%d]: id=%d rate=%.4f%s",
                     s_rate_log,
                     id,
                     rate,
                     jumped ? " [jump]" : "");
            s_last_logged_rate[id] = rate;
        }
    }

    // Scene-level property setters
    void setClearColor(float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_clear_color = std::array { r, g, b };
    }
    void setBloomStrength(float v) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_bloom_strength = v;
    }
    void setBloomThreshold(float v) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_bloom_threshold = v;
    }
    void setCameraFov(float v) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_camera_fov = v;
    }
    void setCameraLookAt(float ex, float ey, float ez, float cx, float cy, float cz, float ux,
                         float uy, float uz) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_camera_lookat = CameraLookAtUpdate { { (double)ex, (double)ey, (double)ez },
                                                       { (double)cx, (double)cy, (double)cz },
                                                       { (double)ux, (double)uy, (double)uz } };
    }
    void setAmbientColor(float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_ambient_color = std::array { r, g, b };
    }
    void setSkylightColor(float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_skylight_color = std::array { r, g, b };
    }
    void setLightColor(i32 index, float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_light_colors.push_back({ index, { r, g, b } });
    }
    void setLightRadius(i32 index, float v) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_light_radii.push_back({ index, v });
    }
    void setLightIntensity(i32 index, float v) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_light_intensities.push_back({ index, v });
    }
    void setLightPosition(i32 index, float x, float y, float z) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_light_positions.push_back({ index, { x, y, z } });
    }

    // Batched layer-update apply: takes the property-update mutex once for
    // the whole batch instead of per-property.  For scenes like 3body
    // (3509243656) with 1200 dirty pool layers per tick × ~3 properties, this
    // collapses ~3600 lock/unlock pairs into one.  Flags use the same
    // bitmask as the JS DIRTY_STRIDE layout (F_ORIGIN=1, F_SCALE=2, etc).
    void applyLayerBatch(const std::vector<SceneWallpaper::LayerBatchUpdate>& batch) {
        static constexpr u32 F_ORIGIN = 1, F_SCALE = 2, F_ANGLES = 4, F_VISIBLE = 8, F_ALPHA = 16;
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        for (const auto& e : batch) {
            if (e.flags & F_ORIGIN) {
                m_pending_transform_updates[{ e.id, std::string("origin") }] = { e.origin[0],
                                                                                 e.origin[1],
                                                                                 e.origin[2] };
            }
            if (e.flags & F_SCALE) {
                m_pending_transform_updates[{ e.id, std::string("scale") }] = { e.scale[0],
                                                                                e.scale[1],
                                                                                e.scale[2] };
            }
            if (e.flags & F_ANGLES) {
                m_pending_transform_updates[{ e.id, std::string("angles") }] = { e.angles[0],
                                                                                 e.angles[1],
                                                                                 e.angles[2] };
            }
            if (e.flags & F_VISIBLE) {
                m_pending_visible_updates[e.id] = e.visible != 0;
            }
            if (e.flags & F_ALPHA) {
                m_pending_alpha_updates[e.id] = e.alpha;
            }
        }
    }

private:
    MHANDLER_CMD(STOP) {
        bool stop { false };
        if (msg->findBool("value", &stop)) {
            if (stop)
                frame_timer.Stop();
            else
                frame_timer.Run();
        }
    }
    MHANDLER_CMD(DRAW) {
        WEK_PROFILE_SCOPE("RenderHandler::DRAW");
        // If device was lost, attempt recovery
        if (m_render->deviceLost()) {
            recoverFromDeviceLost();
            return;
        }

        // Render-thread-confined: .load() the atomic scene pointer once into a
        // local and use `scene` for the rest of the frame.  The render looper
        // is the only writer (CMD_SET_SCENE/HDR/STOP/device-lost), so this load
        // is uncontended here, but the atomic type forces a single deref point.
        auto scene = m_scene.load();

        // Apply queued parent-change requests from SceneScript before any
        // transform / visibility traversal.  See layer-hierarchy spec for
        // the JS-side queue API and cycle prevention.  No-op when no scene
        // is loaded yet.  Apply child-sort drain in the same window so
        // sortLayer + setLayerParent run on the same frame.
        if (scene) {
            scene->ApplyPendingParentChanges();
            scene->ApplyPendingChildSorts();
        }

        frame_timer.FrameBegin();
        if (m_rg) {
            // LOG_INFO("frame info, fps: %.1f, frametime: %.1f", 1.0f, 1000.0f*scene->frameTime);
            scene->shaderValueUpdater->FrameBegin();
            {
                auto pos = m_mouse_pos.load();
                scene->shaderValueUpdater->MouseInput(pos[0], pos[1]);

                // Update particle control points that follow the mouse
                auto* wpUpdater =
                    static_cast<WPShaderValueUpdater*>(scene->shaderValueUpdater.get());
                auto mousePos = wpUpdater->GetMousePosition();
                scene->paritileSys->UpdateMouseControlPoints(mousePos,
                                                             { scene->ortho[0], scene->ortho[1] });
            }

            // Per-frame volumetric tick: recompute is_inside_this_frame from
            // the live camera and push per-light uniforms onto the volumetric
            // surface materials.  Cheap no-op when volumetricsConfig.enabled
            // is false (the vast majority of scenes); kept here — between the
            // mouse-particle drain and Emitt() — so the render-graph rebuild
            // downstream sees up-to-date is_inside_this_frame when it picks
            // front vs fullscreen for each light this frame.
            {
                auto* wpUpdater =
                    static_cast<WPShaderValueUpdater*>(scene->shaderValueUpdater.get());
                if (wpUpdater) {
                    vulkan::PumpVolumetricFrame(*scene, *wpUpdater);
                }
            }

            // Audio-reactive emit-rate push + particle Emitt are now driven
            // by the sub-tick loop below (after dt_scene is computed).  The
            // loop subdivides a stretched (TTY-switch / suspend wake) frame
            // into <= 32ms chunks so every per-frame reader of scene.frameTime
            // (CP velocity divisor at ParticleSystem.cpp:180, trail timestamps
            // at ParticleSystem.cpp:485, g_Time shader uniform, camera path
            // animation) sees a consistent clock.  The push + Emitt MUST run
            // inside each sub-step so audio multiplier + emission rate stay in
            // lockstep with the per-step dt.

            // Auto-hide pool particle nodes whose burst has played out.
            // SceneScript pool-particle assets (e.g. dino_run's coinget)
            // fire their instantaneous emitter on first tick after
            // createLayer, particles live their lifetime, then die.  If the
            // script doesn't promptly call destroyLayer, the node stays
            // "visible" with zero active particles.  Hide it here so pool
            // slots release automatically; JS destroyLayer then re-pushes
            // the name to the pool for reuse.
            for (auto& [nodeId, sub] : scene->particleSubByNodeId) {
                if (! sub || ! sub->IsBurstDone()) continue;
                auto nit = scene->nodeById.find(nodeId);
                if (nit == scene->nodeById.end() || ! nit->second) {
                    sub->ClearBurstDone();
                    continue;
                }
                if (nit->second->IsVisible()) {
                    nit->second->SetVisible(false);
                }
                sub->ClearBurstDone();
            }

            // Process pending text updates before drawing
            {
                std::lock_guard<std::mutex> lock(m_text_update_mutex);
                // Pointsize updates first — they influence every subsequent
                // rasterization for the same layer in this frame.
                for (auto& [id, newSize] : m_pending_pointsize_updates) {
                    for (auto& tl : scene->textLayers) {
                        if (tl.id != id) continue;
                        if (newSize > 0.0f && std::abs(tl.pointsize - newSize) > 0.01f) {
                            tl.pointsize      = newSize;
                            tl.pointsizeDirty = true;
                        }
                        break;
                    }
                }
                // Style updates (halign/valign/font) — applied before text
                // updates so a write-through bundle ('thisLayer.font = X;
                // thisLayer.text = Y;') uses the new font for rasterization.
                // Font name → bytes is resolved here on the render thread
                // because the VFS lives on the scene struct we already own.
                for (auto& [id, style] : m_pending_text_style_updates) {
                    for (auto& tl : scene->textLayers) {
                        if (tl.id != id) continue;
                        // Font name -> bytes is resolved here on the render thread
                        // because the VFS lives on the scene struct we already own;
                        // a miss is logged and leaves the font unchanged.
                        auto resolveFont = [&](const std::string& name) -> std::string {
                            std::string bytes;
                            if (scene->vfs) {
                                if (scene->vfs->Contains("/assets/" + name))
                                    bytes = fs::GetFileContent(*scene->vfs, "/assets/" + name);
                                else if (scene->vfs->Contains("/" + name))
                                    bytes = fs::GetFileContent(*scene->vfs, "/" + name);
                            }
                            if (bytes.empty())
                                LOG_ERROR("setTextStyle: font '%s' not found in VFS", name.c_str());
                            return bytes;
                        };
                        TextStyleTarget tgt { tl.halign, tl.valign, tl.fontName, tl.fontData };
                        if (applyTextStyle(style, tgt, resolveFont)) {
                            tl.halign         = std::move(tgt.halign);
                            tl.valign         = std::move(tgt.valign);
                            tl.fontName       = std::move(tgt.fontName);
                            tl.fontData       = std::move(tgt.fontData);
                            tl.textStyleDirty = true;
                        }
                        break;
                    }
                }
                for (auto& [id, newText] : m_pending_text_updates) {
                    for (auto& tl : scene->textLayers) {
                        if (tl.id != id) continue;
                        // Re-rasterize if text, pointsize, or style changed
                        if (tl.currentText == newText && ! tl.pointsizeDirty
                            && ! tl.textStyleDirty)
                            break;
                        auto img = WPTextRenderer::RenderText(tl.fontData,
                                                              tl.pointsize,
                                                              newText,
                                                              tl.texWidth,
                                                              tl.texHeight,
                                                              tl.halign,
                                                              tl.valign,
                                                              tl.padding);
                        if (img) {
                            img->key = tl.textureKey;
                            m_render->reuploadTexture(tl.textureKey, *img);
                            tl.currentText    = newText;
                            tl.pointsizeDirty = false;
                            tl.textStyleDirty = false;
                        }
                        break;
                    }
                }
                // Style-only updates: layers whose style changed but had no
                // pending text update still need a re-render against the
                // existing currentText.  (Without this pass, halign/font
                // changes wouldn't manifest until the next text mutation.)
                for (auto& tl : scene->textLayers) {
                    if (! tl.textStyleDirty) continue;
                    if (tl.currentText.empty()) {
                        tl.textStyleDirty = false;
                        continue;
                    }
                    auto img = WPTextRenderer::RenderText(tl.fontData,
                                                          tl.pointsize,
                                                          tl.currentText,
                                                          tl.texWidth,
                                                          tl.texHeight,
                                                          tl.halign,
                                                          tl.valign,
                                                          tl.padding);
                    if (img) {
                        img->key = tl.textureKey;
                        m_render->reuploadTexture(tl.textureKey, *img);
                        tl.textStyleDirty = false;
                    }
                }
                m_pending_text_updates.clear();
                m_pending_pointsize_updates.clear();
                m_pending_text_style_updates.clear();
            }

            // Process video texture frame updates — only decode visible videos
            for (auto& vd : m_video_decoders) {
                if (! vd.decoder) continue;
                // Visibility gating: a single MP4 may be sampled by many
                // layers (e.g. 3276911872's morning5/day5/dusk5/night5 plus
                // the "1"/"2"/"3"/"4" variants under 精细).  Play as long as
                // ANY sampler is visible; pause only when all are hidden.
                bool visible = false;
                if (! vd.ownerNode && vd.ownerNodes.empty()) {
                    visible = true; // no owner info — assume always-on
                } else {
                    if (vd.ownerNode && vd.ownerNode->IsVisible()) visible = true;
                    for (auto* n : vd.ownerNodes)
                        if (n && n->IsVisible()) {
                            visible = true;
                            break;
                        }
                }
                if (! visible) {
                    if (vd.decoder->isPlaying()) vd.decoder->pause();
                    continue;
                }
                if (! vd.decoder->isPlaying()) vd.decoder->play();
                if (! vd.decoder->hasNewFrame()) continue;
                const uint8_t* frameData = vd.decoder->acquireFrame();
                if (! frameData) continue;

                Image img;
                img.key              = vd.textureKey;
                img.header.format    = TextureFormat::RGBA8;
                img.header.width     = vd.decoder->width();
                img.header.height    = vd.decoder->height();
                img.header.mapWidth  = vd.decoder->width();
                img.header.mapHeight = vd.decoder->height();
                img.header.count     = 1;

                Image::Slot slot;
                slot.width  = vd.decoder->width();
                slot.height = vd.decoder->height();
                ImageData mip;
                mip.width  = vd.decoder->width();
                mip.height = vd.decoder->height();
                mip.size   = vd.decoder->width() * vd.decoder->height() * 4;
                // Use non-owning pointer — data is owned by the decoder's triple buffer
                mip.data = ImageDataPtr(const_cast<uint8_t*>(frameData), [](uint8_t*) {
                });
                slot.mipmaps.push_back(std::move(mip));
                img.slots.push_back(std::move(slot));

                m_render->reuploadTexture(vd.textureKey, img);
                vd.decoder->releaseFrame();
            }

            // Process pending color updates before drawing
            {
                std::lock_guard<std::mutex> lock(m_color_update_mutex);
                if (! m_pending_color_updates.empty()) {
                    LOG_INFO("DRAW: %zu pending color updates, %zu colorScripts",
                             m_pending_color_updates.size(),
                             scene->colorScripts.size());
                }
                for (auto& [id, rgb] : m_pending_color_updates) {
                    for (auto& cs : scene->colorScripts) {
                        if (cs.id == id && cs.material) {
                            // Preserve existing alpha from g_Color4
                            float alpha = 1.0f;
                            auto  it    = cs.material->customShader.constValues.find("g_Color4");
                            if (it != cs.material->customShader.constValues.end() &&
                                it->second.size() >= 4) {
                                alpha = it->second[3];
                            }
                            cs.material->customShader.constValues["g_Color4"] =
                                std::vector<float> { rgb[0], rgb[1], rgb[2], alpha };
                            cs.material->customShader.constValuesDirty = true;
                            LOG_INFO("color update id=%d: rgb=(%.3f,%.3f,%.3f) alpha=%.3f",
                                     id,
                                     rgb[0],
                                     rgb[1],
                                     rgb[2],
                                     alpha);
                            break;
                        }
                    }
                }
                m_pending_color_updates.clear();
            }

            // Process pending property script updates (transform, visibility, alpha)
            {
                std::lock_guard<std::mutex> lock(m_property_update_mutex);
                static int                  drawDiagCount = 0;
                if (m_drawDiagReset) {
                    drawDiagCount   = 0;
                    m_drawDiagReset = false;
                }
                bool logDiag = (++drawDiagCount % 180 == 1); // every ~6 sec at 30fps
                if (logDiag) {
                    LOG_INFO("DRAW: pending transforms=%zu visible=%zu alpha=%zu nodeById=%zu",
                             m_pending_transform_updates.size(),
                             m_pending_visible_updates.size(),
                             m_pending_alpha_updates.size(),
                             scene->nodeById.size());
                }
                int transformHit = 0, transformMiss = 0;
                int effectRedirects   = 0;
                int sampleCount       = 0;
                int planetSampleCount = 0;
                for (auto& [key, vec] : m_pending_transform_updates) {
                    auto [id, prop] = key;
                    auto nit        = scene->nodeById.find(id);
                    if (nit == scene->nodeById.end()) {
                        transformMiss++;
                        continue;
                    }
                    transformHit++;
                    SceneNode*      node = nit->second;
                    Eigen::Vector3f v(vec[0], vec[1], vec[2]);
                    // Sample first 5 transforms + planet-range transforms
                    if (logDiag && sampleCount < 5) {
                        sampleCount++;
                        LOG_INFO("DRAW sample: id=%d prop=%s val=(%.4f, %.4f, %.4f)",
                                 id,
                                 prop.c_str(),
                                 vec[0],
                                 vec[1],
                                 vec[2]);
                    }
                    if (logDiag && id >= 1360 && id <= 1400 && planetSampleCount < 10) {
                        planetSampleCount++;
                        LOG_INFO("DRAW planet: id=%d prop=%s val=(%.4f, %.4f, %.4f) visible=%d",
                                 id,
                                 prop.c_str(),
                                 vec[0],
                                 vec[1],
                                 vec[2],
                                 (int)node->IsVisible());
                    }
                    // For nodes with effect chains, redirect transform updates
                    // to the resolved final composite node.  The world node must
                    // stay at identity so the base render fills the ping-pong RT.
                    auto       eit            = scene->nodeEffectLayerMap.find(id);
                    SceneNode* resolvedOutput = nullptr;
                    if (eit != scene->nodeEffectLayerMap.end()) {
                        resolvedOutput = eit->second->ResolvedLastOutput();
                        effectRedirects++;
                    }

                    // For compose layers, the worldNode kept its parse-time
                    // transform (WPSceneParser skips the identity-reset for
                    // isCompose).  Its MVP feeds the composelayer base pass,
                    // which samples _rt_default at the layer's screen position
                    // via the composelayer shader's world-NDC math — so the
                    // base capture position must track the final draw position
                    // each frame.  Without this, a scripted oscillating origin
                    // leaves the pingpong holding FB pixels from the parse-time
                    // origin while the final draw renders at the current
                    // scripted origin → visible rectangular ghost (Clair Obscur
                    // Expedition 33 3498984739 M2 compose, ~30px scene shift
                    // per frame).  IsComposeLayer covers ALL compose layers,
                    // unlike IsPassthrough which only fires when copybackground
                    // is false or scene.json sets config.passthrough.
                    const bool composeWorldTracks =
                        resolvedOutput && eit->second->IsComposeLayer();
                    if (prop == "origin") {
                        if (resolvedOutput) {
                            resolvedOutput->SetTranslate(v);
                        }
                        if (! resolvedOutput || composeWorldTracks) {
                            node->SetTranslate(v);
                        }
                    } else if (prop == "scale") {
                        if (resolvedOutput) {
                            resolvedOutput->SetScale(v);
                        }
                        if (! resolvedOutput || composeWorldTracks) {
                            node->SetScale(v);
                        }
                    } else if (prop == "angles") {
                        // WE SceneScript outputs angles in degrees; Eigen AngleAxis expects radians
                        constexpr float deg2rad = M_PI / 180.0f;
                        Eigen::Vector3f rv(v[0] * deg2rad, v[1] * deg2rad, v[2] * deg2rad);
                        if (resolvedOutput) {
                            resolvedOutput->SetRotation(rv);
                        }
                        if (! resolvedOutput || composeWorldTracks) {
                            node->SetRotation(rv);
                        }
                    }
                }
                int visHit = 0, visMiss = 0;
                for (auto& [id, visible] : m_pending_visible_updates) {
                    auto nit = scene->nodeById.find(id);
                    if (nit != scene->nodeById.end()) {
                        visHit++;
                        bool wasVisible = nit->second->IsVisible();
                        nit->second->SetVisible(visible);
                        // Pool-particle rearm: on a false→true transition,
                        // reset the associated particle subsystem so its
                        // burst emitters refire.  Without this, a pool'd
                        // burst particle (e.g. dino_run's coinget) only
                        // fires its instantaneous emit on the first frame
                        // after scene load and never again.
                        if (visible && ! wasVisible) {
                            auto pit = scene->particleSubByNodeId.find(id);
                            if (pit != scene->particleSubByNodeId.end() && pit->second) {
                                pit->second->Reset();
                            }
                        }
                        // Diagnostic for dynamic-asset pool nodes.  Range
                        // allows for hint-sized pools (3body trails ~1200).
                        if (id >= 2'000'000 && id < 2'020'000) {
                            LOG_INFO("POOL visible apply: id=%d visible=%d translate=(%.1f,%.1f)",
                                     id,
                                     (int)visible,
                                     nit->second->Translate().x(),
                                     nit->second->Translate().y());
                        }
                    } else {
                        visMiss++;
                    }
                }
                for (auto& [id, alpha] : m_pending_alpha_updates) {
                    auto nit = scene->nodeById.find(id);
                    if (nit == scene->nodeById.end()) continue;
                    SceneNode* node = nit->second;
                    if (node->HasMaterial()) {
                        auto* mat                                    = node->Mesh()->Material();
                        mat->customShader.constValues["g_UserAlpha"] = std::vector<float> { alpha };
                        mat->customShader.constValuesDirty           = true;
                    }
                }
                // Scripted particle instance-override rate — write through
                // to the corresponding ParticleSubSystem's dynamic multiplier.
                // Misses (id that isn't a particle layer) are silently skipped
                // instead of warned: the extractor only emits this channel
                // for particle hosts, so a miss means the node was never
                // created (effect redirect, pool carve-out) and the script
                // just had nothing to drive.
                for (auto& [id, rate] : m_pending_particle_rate) {
                    auto sit = scene->particleSubByNodeId.find(id);
                    if (sit == scene->particleSubByNodeId.end() || ! sit->second) continue;
                    sit->second->SetDynamicRateMultiplier((double)rate);
                }
                m_pending_particle_rate.clear();
                // Apply effect visibility changes
                for (auto& [nodeId, effIdx, vis] : m_pending_effect_visible) {
                    auto eit = scene->nodeEffectLayerMap.find(nodeId);
                    if (eit == scene->nodeEffectLayerMap.end()) continue;
                    auto* effLayer = eit->second;
                    if (effIdx < 0 || effIdx >= (i32)effLayer->EffectCount()) continue;
                    auto& eff    = effLayer->GetEffect(effIdx);
                    eff->visible = vis;
                    for (auto& en : eff->nodes) {
                        en.sceneNode->SetVisible(vis);
                    }
                }
                m_pending_effect_visible.clear();
                // Apply material uniform updates from SceneScript.  Resolve
                // the uniform name through the shader's alias map: scripts
                // write author-facing names ("color", "alpha") via the proxy's
                // _materialPropertyAliases, but the GLSL uniform may be
                // shader-specific (g_Color, g_TintColor, etc.).  Without
                // resolution the write lands in an unused constValues slot.
                for (auto& [nodeId, uName, floats] : m_pending_material_values) {
                    auto nit = scene->nodeById.find(nodeId);
                    if (nit == scene->nodeById.end()) continue;
                    auto* mesh = nit->second->Mesh();
                    if (! mesh) continue;
                    auto* mat = mesh->Material();
                    if (! mat) continue;
                    auto&             aliasMap = mat->customShader.alias;
                    const std::string resolved =
                        aliasMap.count(uName) ? aliasMap.at(uName) : uName;
                    mat->customShader.constValues[resolved] =
                        ShaderValue(floats.data(), floats.size());
                    mat->customShader.constValuesDirty = true;
                }
                m_pending_material_values.clear();
                // Per-effect material writes — same shape, but address the
                // effect chain's nth effect's first node material so scripts
                // like Game Of Life (3453251764) can paint cells via
                // thisLayer.getEffect('paint').getMaterial().color = Vec3(...)
                for (auto& [nodeId, effectIdx, uName, floats] : m_pending_effect_material_values) {
                    auto eit = scene->nodeEffectLayerMap.find(nodeId);
                    if (eit == scene->nodeEffectLayerMap.end() || ! eit->second) continue;
                    auto* layer = eit->second;
                    if (effectIdx < 0 || effectIdx >= (i32)layer->EffectCount()) continue;
                    auto& eff = layer->GetEffect(effectIdx);
                    if (! eff || eff->nodes.empty()) continue;
                    auto& fn = eff->nodes.front();
                    if (! fn.sceneNode || ! fn.sceneNode->HasMaterial()) continue;
                    auto* mat = fn.sceneNode->Mesh()->Material();
                    if (! mat) continue;
                    auto&             aliasMap = mat->customShader.alias;
                    const std::string resolved =
                        aliasMap.count(uName) ? aliasMap.at(uName) : uName;
                    mat->customShader.constValues[resolved] =
                        ShaderValue(floats.data(), floats.size());
                    mat->customShader.constValuesDirty = true;
                }
                m_pending_effect_material_values.clear();
                // Sprite-frame pins from SceneScript setFrame() — drained
                // into Scene::nodeSpriteFrame for per-pass consumption in
                // WPShaderValueUpdater (see SetManualFrame loop there).
                for (auto& [nodeId, pin] : m_pending_sprite_frame) {
                    scene->nodeSpriteFrame[nodeId] = pin;
                }
                m_pending_sprite_frame.clear();
                if (logDiag) {
                    LOG_INFO("DRAW: transform hit=%d miss=%d effectRedirects=%d, visible hit=%d "
                             "miss=%d, alpha=%zu",
                             transformHit,
                             transformMiss,
                             effectRedirects,
                             visHit,
                             visMiss,
                             m_pending_alpha_updates.size());
                    // Dump world transforms for key planet nodes after applying updates
                    for (int checkId : { 1360, 1365, 1373, 1374, 1375, 1376 }) {
                        auto nit = scene->nodeById.find(checkId);
                        if (nit != scene->nodeById.end()) {
                            auto* n = nit->second;
                            n->UpdateTrans();
                            auto  wt = n->ModelTrans();
                            auto& t  = n->Translate();
                            auto& s  = n->Scale();
                            LOG_INFO("NODE %d: trans=(%.3f,%.3f,%.3f) scale=(%.3f,%.3f,%.3f) "
                                     "visible=%d world[0]=(%.4f,%.4f,%.4f,%.4f)",
                                     checkId,
                                     t.x(),
                                     t.y(),
                                     t.z(),
                                     s.x(),
                                     s.y(),
                                     s.z(),
                                     (int)n->IsVisible(),
                                     wt(0, 0),
                                     wt(0, 1),
                                     wt(0, 2),
                                     wt(0, 3));
                        }
                    }
                }
                m_pending_transform_updates.clear();
                m_pending_visible_updates.clear();
                m_pending_alpha_updates.clear();

                // Scene-level property updates
                if (m_pending_clear_color) {
                    scene->clearColor = *m_pending_clear_color;
                    m_pending_clear_color.reset();
                }
                if (m_pending_bloom_strength) {
                    scene->bloomConfig.strength = *m_pending_bloom_strength;
                    if (! scene->bloomConfig.nodes.empty()) {
                        auto* mat = scene->bloomConfig.nodes[0]->Mesh()->Material();
                        mat->customShader.constValues["bloomstrength"] =
                            std::vector<float> { *m_pending_bloom_strength };
                        mat->customShader.constValuesDirty = true;
                    }
                    m_pending_bloom_strength.reset();
                }
                if (m_pending_bloom_threshold) {
                    scene->bloomConfig.threshold = *m_pending_bloom_threshold;
                    if (! scene->bloomConfig.nodes.empty()) {
                        auto* mat = scene->bloomConfig.nodes[0]->Mesh()->Material();
                        mat->customShader.constValues["bloomthreshold"] =
                            std::vector<float> { *m_pending_bloom_threshold };
                        mat->customShader.constValuesDirty = true;
                    }
                    m_pending_bloom_threshold.reset();
                }
                if (m_pending_camera_fov && scene->activeCamera) {
                    scene->activeCamera->SetFov(*m_pending_camera_fov);
                    scene->activeCamera->Update();
                    m_pending_camera_fov.reset();
                }
                if (m_pending_camera_lookat && scene->activeCamera) {
                    auto&           u = *m_pending_camera_lookat;
                    Eigen::Vector3d eye(u.eye[0], u.eye[1], u.eye[2]);
                    Eigen::Vector3d ctr(u.center[0], u.center[1], u.center[2]);
                    Eigen::Vector3d up(u.up[0], u.up[1], u.up[2]);
                    scene->activeCamera->SetDirectLookAt(eye, ctr, up);
                    m_pending_camera_lookat.reset();
                }
                if (m_pending_ambient_color) {
                    scene->ambientColor = *m_pending_ambient_color;
                    m_pending_ambient_color.reset();
                }
                if (m_pending_skylight_color) {
                    scene->skylightColor = *m_pending_skylight_color;
                    m_pending_skylight_color.reset();
                }
                for (auto& u : m_pending_light_colors) {
                    if (u.index >= 0 && u.index < (i32)scene->lights.size()) {
                        scene->lights[u.index]->setColor(
                            Eigen::Vector3f(u.color[0], u.color[1], u.color[2]));
                    }
                }
                m_pending_light_colors.clear();
                for (auto& u : m_pending_light_radii) {
                    if (u.index >= 0 && u.index < (i32)scene->lights.size())
                        scene->lights[u.index]->setRadius(u.value);
                }
                m_pending_light_radii.clear();
                for (auto& u : m_pending_light_intensities) {
                    if (u.index >= 0 && u.index < (i32)scene->lights.size())
                        scene->lights[u.index]->setIntensity(u.value);
                }
                m_pending_light_intensities.clear();
                for (auto& u : m_pending_light_positions) {
                    if (u.index >= 0 && u.index < (i32)scene->lights.size()) {
                        auto* node = scene->lights[u.index]->node();
                        if (node)
                            node->SetTranslate(
                                Eigen::Vector3f(u.position[0], u.position[1], u.position[2]));
                    }
                }
                m_pending_light_positions.clear();
            }

            // Advance scene clock and animation tracks by the true
            // wall-clock delta since the last DRAW, clamped to [0, 100 ms].
            // Using FrameTimer::IdeaTime() here double-counted time when
            // DRAWs queued up (scene load, busy queue draining) — each
            // catch-up DRAW ran in milliseconds but advanced sim time by a
            // full ideatime, making animations race ahead on startup and
            // any time the render loop fell behind.  Wall-clock delta is
            // perception-correct; the upper clamp keeps a long pause
            // (suspend/resume, long scene reload) from jumping the scene
            // clock by many seconds in one frame.
            //
            // Deterministic mode (default OFF) replaces this wall-clock read
            // with a FIXED step so the scene clock, property animations, and
            // particle stepping all advance reproducibly frame-to-frame — the
            // single highest-leverage change for golden-image capture (spec
            // D11 phase-1).  The off-path below is byte-identical to before:
            // when the flag is off we still read steady_clock, clamp, and
            // update m_last_draw_wall_time exactly as in normal playback.
            double dt_wall;
            if (m_init_info.deterministic) {
                dt_wall = m_init_info.fixed_dt;
            } else {
                auto now              = std::chrono::steady_clock::now();
                dt_wall               = m_last_draw_wall_time
                                            ? std::chrono::duration<double>(now - *m_last_draw_wall_time).count()
                                            : frame_timer.IdeaTime();
                m_last_draw_wall_time = now;
                if (dt_wall < 0.0) dt_wall = 0.0;
                if (dt_wall > 0.1) dt_wall = 0.1;
            }
            double dt_scene =
                SelectFrameDt(m_init_info.deterministic, m_init_info.fixed_dt, dt_wall) * m_speed;

            // Tick property animations + audio-reactive push + PassFrameTime
            // + particle Emitt in <= 32ms sub-steps so every consumer of the
            // scene clock sees the same per-step value.  Previously dt_scene
            // could be up to 100ms (wall-clock clamp) while Emitt's internal
            // clamp limited emission to 32ms — diverging the scene clock from
            // the particle clock on any stalled frame (TTY switch, suspend
            // wake, compile first frame).  Now one clock: scene.frameTime
            // bounded per step; scene.elapsingTime accumulates to full
            // dt_scene across sub-steps (so g_Time stays on the wall clock).
            //
            // drawFrame + the post-draw bookkeeping (worldCacheRebuild,
            // deviceLost, m_scene_time.store) run ONCE per render call after
            // the loop — saturating the render thread with N draws on a long
            // stall would defeat the catch-up.
            constexpr double kMaxFixedTick = 0.032;
            auto             subTickPlan   = computeSubTickPlan(dt_scene, kMaxFixedTick);
            for (double step : subTickPlan) {
                tickPropertyAnimations(step);

                // Audio-reactive emit-rate push.  For each subsystem whose
                // source emitter authored audioprocessingmode != 0, sample
                // the bass band of the FFT spectrum and push a multiplier
                // into Emitt's rate_eff.  Smoothing state lives on the
                // subsystem so attack/decay survives across frames.  Skipped
                // entirely (no map walk, no spectrum span fetch) when no
                // subsystem is audio-reactive (the vast majority of scenes);
                // the flag is OR'd over particleSubByNodeId once at scene
                // build.
                if (scene->hasAudioReactiveParticles) {
                    auto                   analyzer = scene->audioAnalyzer;
                    std::span<const float> specLeft = analyzer && analyzer->HasData()
                                                          ? analyzer->GetRawSpectrum(16, 0)
                                                          : std::span<const float> {};
                    std::span<const float> specRight = analyzer && analyzer->HasData()
                                                           ? analyzer->GetRawSpectrum(16, 1)
                                                           : std::span<const float> {};
                    for (auto& [nodeId, sub] : scene->particleSubByNodeId) {
                        if (! sub || ! sub->IsAudioReactive()) continue;
                        auto r = audio_reactive::computeRateMultiplier(specLeft,
                                                                       specRight,
                                                                       sub->AudioSmoothedRef(),
                                                                       step,
                                                                       sub->AudioParams());
                        sub->AudioSmoothedRef() = r.newSmoothed;
                        sub->SetAudioRateMultiplier(r.multiplier);
                    }
                }

                // PassFrameTime BEFORE Emitt so Emitt's read of
                // scene.frameTime sees the sub-step value (<= kMaxFixedTick).
                scene->PassFrameTime(step);
                scene->paritileSys->Emitt();
            }

            // Frame-exact screenshot: when a target frame is armed and the
            // monotonic counter has reached it, set the readback path NOW so
            // THIS drawFrame captures that exact scene-frame.  ShouldCaptureAtFrame
            // keeps the off-by-one / disabled-sentinel logic pure + unit-tested.
            if (ShouldCaptureAtFrame(m_shot_at_frame.load(std::memory_order_acquire),
                                     m_frame_idx.load(std::memory_order_acquire))) {
                std::string p;
                {
                    std::lock_guard<std::mutex> lk(m_shot_at_mutex);
                    p = m_shot_at_path;
                }
                m_render->setScreenshotPath(p);
                m_shot_at_frame.store(-1, std::memory_order_release);
            }

            m_render->drawFrame(*scene);

            // Snapshot each named layer's world transform after the per-frame
            // UpdateTrans walk (driven from drawFrame).  SceneScript
            // thisLayer.getTransformMatrix() and hit-test read from this
            // cache off the GUI thread; populating it here keeps the read
            // path lock-light.
            //
            // For effect-having layers spImgNode's parent was disconnected
            // for base-pass rendering (its ModelTrans returns local only),
            // so prefer imgEffectLayer.FinalNode().ModelTrans() when an
            // effect chain exists — that's the node the layer actually
            // renders at on screen, and what hit-test should match.  Game
            // of Life Tool Selection / Color / Stamp buttons all fall in
            // this category, and the previous local-only snapshot put
            // their hit rects at the wrong world position.
            // only rebuild when a SceneScript has read the world cache
            // at least once.  Non-hit-testing scenes (the vast majority) skip the
            // O(named-nodes) UpdateTrans() walk + map churn entirely.  When it IS
            // needed, update in place (operator[] via worldCacheAssign) instead
            // of clear()+reserve()+emplace() so the stable steady-state node set
            // never reallocates and a concurrent reader never sees a transient
            // identity gap for a known id.  The cache + this gate are reset on
            // CMD_SET_SCENE so a recycled id never returns the prior scene's
            // matrix.
            if (worldCacheShouldRebuild(m_needs_world_cache)) {
                std::lock_guard<std::mutex> lk(m_world_cache_mutex);
                for (auto& [name, id] : scene->nodeNameToId) {
                    auto it = scene->nodeById.find(id);
                    if (it == scene->nodeById.end() || ! it->second) continue;
                    Eigen::Matrix4d wt;
                    auto            eit  = scene->nodeEffectLayerMap.find(id);
                    SceneNode*      live = nullptr;
                    if (eit != scene->nodeEffectLayerMap.end() && eit->second) {
                        // Prefer the resolved last-output node — its parent is
                        // wired to the parent_proxy (which holds the authored
                        // world transform), so its ModelTrans gives the on-
                        // screen world position.  FinalNode itself only carries
                        // the local transform; spImgNode's parent was cleared
                        // for base-pass rendering.
                        live = eit->second->ResolvedLastOutput();
                    }
                    if (! live) live = it->second;
                    live->UpdateTrans();
                    wt = live->ModelTrans();
                    std::array<float, 16> arr;
                    // Column-major flat layout matches Eigen's default storage
                    // and WE/GLSL conventions; matrix.m[12..14] are translation.
                    for (int c = 0; c < 4; ++c)
                        for (int r = 0; r < 4; ++r)
                            arr[c * 4 + r] = static_cast<float>(wt(r, c));
                    worldCacheAssign(m_layer_world_cache, id, arr); // in-place reuse
                }
            }

            // Check for device lost after draw
            if (m_render->deviceLost()) {
                LOG_ERROR(
                    "VK_ERROR_DEVICE_LOST detected during drawFrame, will recover next frame");
                frame_timer.FrameEnd();
                return;
            }

            // scene->PassFrameTime is now driven by the sub-tick loop above
            // (one call per sub-step, summing to dt_scene); reading the
            // accumulated elapsingTime here is the canonical export point
            // for SceneScript and friends.
            m_scene_time.store(scene->elapsingTime, std::memory_order_relaxed);

            // DIAG: log elapsingTime vs wall clock drift
            if (std::getenv("WEKDE_TIME_DIAG")) {
                static auto   s_wall_start = std::chrono::steady_clock::now();
                static double s_last_scene = 0.0;
                static int    s_tick_count = 0;
                s_tick_count++;
                if (s_tick_count % 30 == 0) {
                    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                                s_wall_start)
                                      .count();
                    double delta_scene = scene->elapsingTime - s_last_scene;
                    s_last_scene       = scene->elapsingTime;
                    LOG_INFO(
                        "TIME_DIAG tick=%d wall=%.3fs scene=%.3fs ratio=%.3f "
                        "frametime=%.4f ideatime=%.4f required_fps=%d delta30=%.3f dt_wall=%.4f",
                        s_tick_count,
                        wall,
                        scene->elapsingTime,
                        wall > 0.01 ? scene->elapsingTime / wall : 0.0,
                        frame_timer.FrameTime(),
                        frame_timer.IdeaTime(),
                        frame_timer.RequiredFps(),
                        delta_scene,
                        dt_wall);
                }
            }

            scene->shaderValueUpdater->FrameEnd();
            fps_counter.RegisterFrame();

            if (! scene->first_frame_ok) {
                scene->first_frame_ok = true;
                main_handler.sendFirstFrameOk();
            }

            // Bump after a frame is actually drawn (i.e. not the device-lost
            // early-return above).  SceneBackend polls this counter to drive
            // text-script eval once per render frame so wallpaper FPS-display
            // scripts (Miku 3363252053 s13: `1000 / (Date.now() - last)`)
            // measure the real render rate.
            m_frame_idx.fetch_add(1, std::memory_order_release);
        }
        frame_timer.FrameEnd();
    }
    MHANDLER_CMD(SET_FILLMODE) {
        int32_t value;
        if (msg->findInt32("value", &value)) {
            m_fillmode = (FillMode)value;
            auto scene = m_scene.load();
            if (scene && renderInited()) {
                m_render->UpdateCameraFillMode(*scene, m_fillmode);
            }
        }
    }
    MHANDLER_CMD(SET_SCENE) {
        // findObject assigns through a plain shared_ptr<Scene>* and can't bind
        // to the atomic member, so read into a temp then publish via .store().
        // The rest of the body uses the `scene` local (== the just-stored
        // pointer) on the render thread.
        std::shared_ptr<Scene> scene;
        if (msg->findObject("scene", &scene)) {
            // Idle + release the OUTGOING scene's GPU resources BEFORE publishing
            // the new one.  Without this, store(scene) drops the render thread's
            // last ref to the previous scene and runs ~Scene() (freeing its VMA
            // depth/MSAA images) while up to kFramesInFlight (2) frames may still
            // reference them → destroy-while-in-flight (GPUVM fault /
            // VK_ERROR_DEVICE_LOST, validation error under the layers).  Mirrors
            // CMD_SET_HDR (~1559) and recoverFromDeviceLost (~1779):
            // clearLastRenderGraph (which does a device WaitIdle) on the CURRENT
            // scene first, holding prev alive across the idle so its destructor
            // cannot run early, THEN swap.  The per-scene .reset()s inside
            // clearLastRenderGraph are idempotent, so the synchronous ~Scene() the
            // store still triggers finds them already released.  First scene load
            // is safe: m_scene.load() is null, guard skipped.
            if (auto prev = m_scene.load()) {
                if (m_rg) m_render->clearLastRenderGraph(prev.get());
            }
            m_scene.store(scene);
            // drop the previous scene's world-cache entries and reset
            // the needs-cache gate.  In-place operator[] reuse (no per-frame
            // clear()) means a reload must clear here so a recycled id never
            // returns the old scene's matrix; resetting the gate returns a new
            // script-light scene to the zero-per-frame path until its own scripts
            // hit-test.
            {
                std::lock_guard<std::mutex> lk(m_world_cache_mutex);
                m_layer_world_cache.clear();
            }
            m_needs_world_cache.store(false, std::memory_order_relaxed);
            if (m_rg) m_render->clearLastRenderGraph(scene.get());
            m_drawDiagReset = true;        // force DRAW diagnostic on next frame
            m_last_draw_wall_time.reset(); // first DRAW uses ideatime, not the
                                           // wall-clock gap across scene load

            // HDR mode is driven by the scene's intent (project.json hdr:true, or
            // the parser's auto-detection of overbright content such as
            // instanceoverride.brightness>1).  We enable internal RGBA16F RTs +
            // tonemap FinPass regardless of the host's HDR *output* preference:
            //   - host HDR output + scene HDR → RGBA16F RTs, FinPass passthrough (HDR→HDR)
            //   - host SDR output + scene HDR → RGBA16F RTs, FinPass exposure tonemap
            //   - host any           + scene SDR → RGBA8 RTs,   FinPass passthrough
            // Without the tonemap path, overbright scenes (NieR 3633635618 thunderbolts,
            // etc.) additively pile their clamped-to-1.0 channels into pure white.
            const bool scene_wants_hdr = scene->hdrContent;
            const bool effective_hdr   = scene_wants_hdr;
            if (effective_hdr) {
                int upgraded = 0;
                for (auto& [name, rt] : scene->renderTargets) {
                    if (rt.format == TextureFormat::RGBA8) {
                        rt.format = TextureFormat::RGBA16F;
                        upgraded++;
                    }
                }
                LOG_INFO("HDR content: upgraded %d render targets to RGBA16F (host_hdr=%d)",
                         upgraded,
                         (int)m_render->hdrContent());
            } else {
                LOG_INFO("HDR content: disabled (scene_hdr=%d, host_hdr=%d)",
                         (int)scene_wants_hdr,
                         (int)m_render->hdrContent());
            }
            scene->hdrContent = effective_hdr;
            // Align FinPass tonemap with the effective HDR mode for this scene.
            // If the mode differs from the previous scene, FinPass is marked for
            // re-prepare so it picks the matching tonemap/passthrough shader.
            m_render->setSceneHdrContent(effective_hdr);

            m_rg = sceneToRenderGraph(*scene);

            if (main_handler.isGenGraphviz()) m_rg->ToGraphviz("graph.dot");
            m_render->compileRenderGraph(*scene, *m_rg);
            m_render->UpdateCameraFillMode(*scene, m_fillmode);

            // Deterministic mode (default OFF): seed the particle PRNG here,
            // on the RENDER THREAD, before any particle is spawned.  This is
            // the thread that runs PreSimulate() below and the per-frame
            // Emitt() in CMD_DRAW, and effolkronium's Random is THREAD-LOCAL —
            // seeding on any other thread (e.g. the main/QML thread) would be a
            // silent no-op that leaves ParticleEmitter's Random::get<> draws
            // nondeterministic.  Seeding here makes every emitter spawn and
            // each particle's stable random_seed reproducible (spec D11
            // phase-1).  The guarded LOG_INFO confirms the seed actually took
            // on this thread (a thread-local-misseed is the exact stub-failure
            // mode the project rejects, so we verify, not assume).
            if (m_init_info.deterministic) {
                Random::seed(m_init_info.rng_seed);
                LOG_INFO("deterministic: seeded particle Random with 0x%08x on render thread",
                         m_init_info.rng_seed);
            }

            // Pre-simulate particle systems so scenes that author a non-zero
            // `starttime` (WE semantic: "seconds of sim before frame 1", e.g.
            // shimmering_particles' 50s dustmotes / 200s small_motes) show a
            // populated scene on load instead of a black screen ramping up.
            if (scene->paritileSys) {
                scene->paritileSys->PreSimulate();
            }

            // Create video texture decoders for MP4 textures detected during loading
            // Try HW decoder first (EGL + GL + VA-API), fall back to SW
            {
                std::lock_guard<std::mutex> lock(m_video_decoders_mutex);
                m_video_decoders.clear();
            }
            for (auto& vt : scene->videoTextures) {
                std::shared_ptr<VideoTextureDecoder> decoder;
#ifdef HAVE_EGL_HWDEC
                {
                    auto hw = std::make_shared<HWVideoTextureDecoder>(vt.width, vt.height);
                    if (hw->open(vt.videoFilePath)) decoder = hw;
                }
#endif
                if (! decoder) {
                    auto sw = std::make_shared<VideoTextureDecoder>(vt.width, vt.height);
                    if (sw->open(vt.videoFilePath)) decoder = sw;
                }
                if (decoder) {
                    std::lock_guard<std::mutex> lock(m_video_decoders_mutex);
                    m_video_decoders.push_back(
                        { vt.textureKey, decoder, vt.ownerNode, vt.ownerNodes });
                }
            }
        }
    }
    MHANDLER_CMD(SET_SPEED) { msg->findFloat("value", &m_speed); }
    MHANDLER_CMD(SET_HDR) {
        bool value { false };
        if (msg->findBool("value", &value)) {
            m_init_info.hdr_output = value;
            // Requires full Vulkan reinit (ExSwapchain format change)
            LOG_INFO("HDR output changed to %s, reinitializing Vulkan", value ? "on" : "off");
            frame_timer.Stop();
            if (auto scene = m_scene.load()) {
                m_render->clearLastRenderGraph(scene.get());
            }
            m_scene.store(nullptr);
            m_rg.reset();
            m_render->destroy();
            m_render = std::make_unique<vulkan::VulkanRender>();
            if (! m_render->init(m_init_info)) {
                LOG_ERROR("Failed to reinitialize Vulkan for HDR toggle");
                return;
            }
            frame_timer.Run();
            main_handler.sendCmdLoadScene();
        }
    }
    MHANDLER_CMD(INIT_VULKAN) {
        std::shared_ptr<RenderInitInfo> info;
        if (msg->findObject("info", &info)) {
            m_init_info = *info;
            // Fold the WEK_DETERMINISTIC env-var override into the struct flag
            // once, here, so the per-frame dt site and the scene-load seed site
            // only ever read m_init_info.deterministic (no repeated getenv in
            // the hot draw loop).  Env is an ad-hoc-debugging convenience; the
            // struct field is the explicit source of truth.
            m_init_info.deterministic = ResolveDeterministic(m_init_info.deterministic);
            if (m_init_info.deterministic) {
                LOG_INFO("deterministic render mode ON (fixed_dt=%g, rng_seed=0x%08x)",
                         m_init_info.fixed_dt,
                         m_init_info.rng_seed);
            }
            m_render->init(m_init_info);

            // inited, callback to laod scene
            main_handler.sendCmdLoadScene();
        }
    }

public:
    // Advance all playing property animations on the current scene and
    // push each track's evaluated value into every material that renders
    // the owner node — source material, effect chain nodes, and final
    // composite.  Required because layers with effects bake alpha into
    // the effect chain's own material copies, so updating only the source
    // leaves the rendered output unchanged.  Runs on the render thread.
    void tickPropertyAnimations(double dt) {
        auto scene = m_scene.load();
        if (! scene) return;
        if (scene->nodePropertyAnimations.empty()) return;
        applyPendingPropertyAnimCommands();
        for (auto& [nodeId, anims] : scene->nodePropertyAnimations) {
            auto       nit        = scene->nodeById.find(nodeId);
            SceneNode* sourceNode = (nit != scene->nodeById.end()) ? nit->second : nullptr;

            for (auto& anim : anims) {
                if (anim.playing) anim.time += dt;
                float value = EvaluatePropertyAnimation(anim, anim.time);
                if (anim.property == "alpha") {
                    writeAlphaToAllMaterials(sourceNode, nodeId, value);
                } else {
                    // Vec3 components: "origin.{x,y,z}", "scale.{x,y,z}",
                    // "angles.{x,y,z}".  In relative mode evaluated value is a
                    // delta added on top of the per-axis base in initialValue;
                    // in absolute mode it replaces the component outright.
                    applyVec3ComponentAnim(sourceNode, nodeId, anim, value);
                }
            }
        }
    }

    // Dispatch a single-axis property animation tick onto the corresponding
    // component of the node's translate / scale / rotation.  Property names
    // carry the axis suffix ("origin.x" etc); we strip the suffix to pick the
    // SceneNode setter, then read-modify-write the chosen axis.  No-ops when
    // the node is missing or the property name doesn't match an axis we
    // recognise (so legacy scalar entries that don't carry a suffix won't
    // accidentally clobber the transform).
    void applyVec3ComponentAnim(SceneNode* node, i32 nodeId, const PropertyAnimation& anim,
                                float evaluated) {
        if (! node) return;
        const std::string& prop = anim.property;
        if (prop.size() < 3) return;
        int axis = -1;
        if (prop.ends_with(".x"))
            axis = 0;
        else if (prop.ends_with(".y"))
            axis = 1;
        else if (prop.ends_with(".z"))
            axis = 2;
        if (axis < 0) return;
        std::string_view base { prop.data(), prop.size() - 2 };

        float target = anim.relative ? (anim.initialValue + evaluated) : evaluated;

        // For layers with an effect chain, the image node's transform must
        // stay at IDENTITY — the base pass renders into a pingpong RT using a
        // per-image ortho camera at world (0,0) sized to the layer, so the
        // local quad fills the pingpong.  Writing world coords to the image
        // node moves the quad off-pingpong, leaving the pingpong empty and the
        // composite invisible (Rella whale 3363252053 had this exact failure
        // mode — the whole creature vanished except a small tail-fin sliver).
        //
        // The COMPOSITE pass uses last_output->sceneNode whose transform is
        // copied from m_final_node at SceneImageEffectLayer::Resolve time.
        // Redirect our writes to that node so origin animations actually
        // shift the composite's screen position.  Fall back to the image
        // node for layers without effects.
        SceneNode* target_node = node;
        if (auto scene = m_scene.load()) {
            auto eit = scene->nodeEffectLayerMap.find(nodeId);
            if (eit != scene->nodeEffectLayerMap.end() && eit->second) {
                if (auto* resolved = eit->second->ResolvedLastOutput()) {
                    target_node = resolved;
                }
            }
        }

        if (base == "origin") {
            Eigen::Vector3f v = target_node->Translate();
            v[axis]           = target;
            target_node->SetTranslate(v);
        } else if (base == "scale") {
            Eigen::Vector3f v = target_node->Scale();
            v[axis]           = target;
            target_node->SetScale(v);
        } else if (base == "angles") {
            Eigen::Vector3f v = target_node->Rotation();
            v[axis]           = target;
            target_node->SetRotation(v);
        }
    }

    // Apply an alpha value to every material participating in the node's
    // render path so layers with effect chains don't render with a stale
    // baked-in alpha from their per-effect material copy.
    void writeAlphaToAllMaterials(SceneNode* sourceNode, i32 nodeId, float value) {
        auto pushAlpha = [value](SceneMaterial* mat) {
            if (! mat) return;
            mat->customShader.constValues["g_UserAlpha"] = std::vector<float> { value };
            // Also update g_Color4.a so shaders that sample color alpha
            // (rather than the explicit g_UserAlpha uniform) pick this up.
            auto it = mat->customShader.constValues.find("g_Color4");
            if (it != mat->customShader.constValues.end() && it->second.size() >= 4) {
                it->second[3] = value;
            }
            mat->customShader.constValuesDirty = true;
        };

        if (sourceNode && sourceNode->HasMaterial()) {
            pushAlpha(sourceNode->Mesh()->Material());
        }
        auto scene = m_scene.load();
        if (! scene) return;
        auto eit = scene->nodeEffectLayerMap.find(nodeId);
        if (eit != scene->nodeEffectLayerMap.end() && eit->second) {
            auto* eff = eit->second;
            for (std::size_t i = 0; i < eff->EffectCount(); i++) {
                auto& e = eff->GetEffect(i);
                for (auto& en : e->nodes) {
                    if (en.sceneNode && en.sceneNode->HasMaterial()) {
                        pushAlpha(en.sceneNode->Mesh()->Material());
                    }
                }
            }
            pushAlpha(eff->FinalMesh().Material());
        }
    }

    void propertyAnimCommand(int32_t nodeId, const std::string& name, const std::string& cmd) {
        std::lock_guard<std::mutex> lock(m_prop_anim_cmds_mutex);
        m_prop_anim_cmds.push_back({ nodeId, name, cmd });
    }

    bool propertyAnimIsPlaying(int32_t nodeId, const std::string& name) const {
        // Called inline on the QML thread; .load() keeps the Scene alive across
        // the lookup so the pointer can't tear under a render-thread reset.
        auto scene = m_scene.load();
        if (! scene) return false;
        auto it = scene->nodePropertyAnimations.find(nodeId);
        if (it == scene->nodePropertyAnimations.end()) return false;
        for (const auto& a : it->second) {
            if (a.name == name) return a.playing;
        }
        return false;
    }

    void applyPendingPropertyAnimCommands() {
        std::vector<PropertyAnimCmd> cmds;
        {
            std::lock_guard<std::mutex> lock(m_prop_anim_cmds_mutex);
            cmds.swap(m_prop_anim_cmds);
        }
        if (cmds.empty()) return;
        auto scene = m_scene.load();
        if (! scene) return;
        for (auto& c : cmds) {
            auto it = scene->nodePropertyAnimations.find(c.nodeId);
            if (it == scene->nodePropertyAnimations.end()) continue;
            for (auto& a : it->second) {
                if (a.name != c.name) continue;
                if (c.cmd == "play") {
                    a.playing = true;
                } else if (c.cmd == "pause") {
                    a.playing = false;
                } else if (c.cmd == "stop") {
                    a.playing = false;
                    a.time    = 0.0;
                }
                break;
            }
        }
    }

private:
    void recoverFromDeviceLost() {
        LOG_INFO("Recovering from VK_ERROR_DEVICE_LOST...");

        // Stop the frame timer during recovery
        frame_timer.Stop();

        // Release Scene GPU resources before destroying VMA allocator
        if (auto scene = m_scene.load()) {
            m_render->clearLastRenderGraph(scene.get());
        }

        // Clear scene state
        m_scene.store(nullptr);
        m_rg.reset();

        // Destroy and recreate the Vulkan renderer
        m_render->destroy();
        m_render = std::make_unique<vulkan::VulkanRender>();

        if (! m_render->init(m_init_info)) {
            LOG_ERROR("Failed to reinitialize Vulkan after device lost, retrying in 1s");
            // Post a delayed draw message to retry
            frame_timer.SetRequiredFps(1);
            frame_timer.Run();
            return;
        }

        LOG_INFO("Vulkan device recreated successfully, reloading scene");
        frame_timer.Run();

        // Trigger scene reload on the main thread
        main_handler.sendCmdLoadScene();
    }

public:
    FrameTimer frame_timer;
    FpsCounter fps_counter;

private:
    // Atomic: written on the render looper thread (CMD_SET_SCENE assign,
    // CMD_SET_HDR / CMD_STOP / device-lost reset) and read on the QML thread
    // (getLayerSpriteSnapshot / getLayerBoneIndex / queueParentChange /
    // queueChildSort / propertyAnimIsPlaying — all called inline, no message
    // post).  Render-thread methods .load() once into a local `scene` and use
    // that; the atomic has no operator-> so any missed deref fails to compile.
    std::atomic<std::shared_ptr<Scene>> m_scene { nullptr };
    float                               m_speed { 1.0f };

    std::unique_ptr<vulkan::VulkanRender> m_render;
    std::unique_ptr<rg::RenderGraph>      m_rg { nullptr };

    FillMode m_fillmode { FillMode::ASPECTCROP };

    RenderInitInfo                    m_init_info;
    std::atomic<std::array<float, 2>> m_mouse_pos { std::array { 0.5f, 0.5f } };

    // Published scene clock (m_scene->elapsingTime) — read cross-thread from QML.
    // Writers: this render thread after each PassFrameTime(). Readers: SceneBackend.
    std::atomic<double> m_scene_time { 0.0 };

    // Wall-clock timestamp of the previous DRAW that advanced scene time.
    // Used to drive PassFrameTime() off real elapsed time rather than the
    // FrameTimer ideatime — when DRAWs queue up at startup, the timer-driven
    // ideatime double-counts sim time and makes animations race ahead.
    std::optional<std::chrono::steady_clock::time_point> m_last_draw_wall_time;

public:
    double   getSceneTime() const { return m_scene_time.load(std::memory_order_relaxed); }
    uint64_t getFrameIdx() const { return m_frame_idx.load(std::memory_order_acquire); }

private:
    // Monotonic frame counter — bumped at the end of every CMD_DRAW that
    // produced a frame (after FrameEnd / shaderValueUpdater->FrameEnd).  Read
    // by SceneBackend (main thread) so text scripts can be gated to fire once
    // per real render frame, which makes WE's "fps = 1000 / (Date.now() - last)"
    // pattern (e.g. Miku 3363252053 s13 frame-rate display script) measure the
    // actual render rate instead of the QTimer cadence.
    std::atomic<uint64_t> m_frame_idx { 0 };

    // Frame-exact screenshot target (frame-indexed capture).  -1 = none.  Set
    // off-thread via requestScreenshotAtFrame(); consumed on the render thread
    // in CMD_DRAW, which arms m_render->setScreenshotPath() exactly when
    // m_frame_idx reaches the target.  This makes warm/cold runs capture the
    // SAME scene-frame regardless of wall-clock / shader-compile time — the
    // byte-identical-comparison prerequisite the wall-clock --screenshot-frames
    // sleep cannot provide.
    std::atomic<int64_t> m_shot_at_frame { -1 };
    std::mutex           m_shot_at_mutex;
    std::string          m_shot_at_path;

    std::mutex                           m_text_update_mutex;
    std::unordered_map<i32, std::string> m_pending_text_updates;
    std::unordered_map<i32, float>       m_pending_pointsize_updates;
    // Per-id queue for thisLayer.horizontalalign/verticalalign/font/alignment.
    // PendingTextStyleUpdate + the merge/apply helpers live in
    // Scene/TextStyleMerge.hpp (wpScene, no Vulkan) so they are unit-testable.
    std::unordered_map<i32, PendingTextStyleUpdate> m_pending_text_style_updates;

    // World-transform cache for SceneScript thisLayer.getTransformMatrix().
    // Populated at the end of every drawFrame; keyed by node id; column-major
    // 16-float matrices.  Mutable so const accessors can lock it.
    mutable std::mutex                                       m_world_cache_mutex;
    std::unordered_map<i32, std::array<float, 16>>           m_layer_world_cache;
    // set true the first time the GUI-thread bridge reads the world
    // cache (thisLayer.getTransformMatrix() / hit-test).  Until then the render
    // thread skips the whole O(named-nodes) rebuild below.  mutable so the const
    // reader can latch it; relaxed ordering — a one-frame delay before the
    // producer observes it is harmless (the reader returns identity for the
    // not-yet-cached id, which is the existing behavior).
    mutable std::atomic<bool> m_needs_world_cache { false };

    std::mutex                                    m_color_update_mutex;
    std::unordered_map<i32, std::array<float, 3>> m_pending_color_updates;

    std::mutex                                                  m_property_update_mutex;
    std::map<std::pair<i32, std::string>, std::array<float, 3>> m_pending_transform_updates;
    std::unordered_map<i32, bool>                               m_pending_visible_updates;
    std::unordered_map<i32, float>                              m_pending_alpha_updates;
    // Scripted particle instance-override rate (NieR:Automata starfields).
    // Merged per-id so the latest value wins within a single tick — there is
    // no reason to replay intermediate values the renderer never sampled.
    std::unordered_map<i32, float> m_pending_particle_rate;
    std::vector<std::tuple<i32, i32, bool>>
        m_pending_effect_visible; // (nodeId, effectIdx, visible)
    // (nodeId, uniformName, floats) — IMaterial.setValue from SceneScript.
    // Drained alongside m_pending_effect_visible; applies to
    // mesh.Material()->customShader.constValues and toggles
    // constValuesDirty so CustomShaderPass re-uploads on the next frame.
    std::vector<std::tuple<i32, std::string, std::vector<float>>>
        m_pending_material_values;
    // (nodeId, effectIdx, uniformName, floats) — IMaterial.setValue from
    // SceneScript via thisLayer.getEffect(name).getMaterial().setValue(...).
    // Drained alongside m_pending_material_values; targets the effect chain's
    // m_effects[effectIdx]'s first node material instead of the main mesh.
    std::vector<std::tuple<i32, i32, std::string, std::vector<float>>>
        m_pending_effect_material_values;
    // nodeId → (wantsManual, frameIdx) from
    // thisLayer.getTextureAnimation().setFrame(N) SceneScript writes.
    // Drained at the start of the render tick into Scene::nodeSpriteFrame;
    // WPShaderValueUpdater consults the Scene-side map per pass since
    // sprites_map copies are scattered across CustomShaderPass instances.
    std::unordered_map<i32, std::pair<bool, i32>> m_pending_sprite_frame;

    // Scene-level pending updates (under m_property_update_mutex)
    std::optional<std::array<float, 3>> m_pending_clear_color;
    std::optional<float>                m_pending_bloom_strength;
    std::optional<float>                m_pending_bloom_threshold;
    std::optional<float>                m_pending_camera_fov;
    struct CameraLookAtUpdate {
        std::array<double, 3> eye, center, up;
    };
    std::optional<CameraLookAtUpdate>   m_pending_camera_lookat;
    std::optional<std::array<float, 3>> m_pending_ambient_color;
    std::optional<std::array<float, 3>> m_pending_skylight_color;
    struct LightColorUpdate {
        i32                  index;
        std::array<float, 3> color;
    };
    struct LightScalarUpdate {
        i32   index;
        float value;
    };
    struct LightPositionUpdate {
        i32                  index;
        std::array<float, 3> position;
    };
    std::vector<LightColorUpdate>    m_pending_light_colors;
    std::vector<LightScalarUpdate>   m_pending_light_radii;
    std::vector<LightScalarUpdate>   m_pending_light_intensities;
    std::vector<LightPositionUpdate> m_pending_light_positions;

    bool m_drawDiagReset { false };

    // Video texture decoders
    struct VideoDecoderEntry {
        std::string                          textureKey;
        std::shared_ptr<VideoTextureDecoder> decoder;
        SceneNode*                           ownerNode { nullptr };
        std::vector<SceneNode*>              ownerNodes;
    };
    std::vector<VideoDecoderEntry> m_video_decoders;
    // Guards m_video_decoders when read from outside the render thread
    // (SceneScript bridge methods: videoPlay / videoGetCurrentTime / ...).
    mutable std::mutex m_video_decoders_mutex;

public:
    // Find the decoder owning a given nodeId. Returns nullptr if no match.
    // Takes a shared_ptr copy under the mutex so the caller can safely use
    // it after releasing the lock.
    std::shared_ptr<VideoTextureDecoder> findVideoDecoder(int32_t nodeId) const {
        std::lock_guard<std::mutex> lock(m_video_decoders_mutex);
        for (auto& vd : m_video_decoders) {
            if (vd.ownerNode && vd.ownerNode->ID() == nodeId) return vd.decoder;
        }
        return nullptr;
    }
    std::vector<int32_t> getVideoNodeIds() const {
        std::lock_guard<std::mutex> lock(m_video_decoders_mutex);
        std::vector<int32_t>        ids;
        ids.reserve(m_video_decoders.size());
        for (auto& vd : m_video_decoders) {
            if (vd.ownerNode) ids.push_back(vd.ownerNode->ID());
        }
        return ids;
    }

private:
    // Queue of script-driven play/stop/pause commands for property animations
    // (layer.getAnimation(name).play() etc.).  Drained at the start of every
    // tickPropertyAnimations() so script intent lands before evaluation.
    struct PropertyAnimCmd {
        int32_t     nodeId;
        std::string name;
        std::string cmd;
    };
    mutable std::mutex           m_prop_anim_cmds_mutex;
    std::vector<PropertyAnimCmd> m_prop_anim_cmds;
};
} // namespace wallpaper

SceneWallpaper::SceneWallpaper(): m_main_handler(std::make_shared<MainHandler>()) {}

SceneWallpaper::~SceneWallpaper() {
    /*
    if(m_offscreen) {
        // no wait
        auto msg = looper::Message::create(0, m_main_handler);
        msg->setObject("self_clean", m_main_handler);
        msg->setCleanAfterDeliver(true);
        m_main_handler = nullptr;
        msg->post();
    }
    */
#ifdef WEK_PROFILING
    // Flush accumulated scope samples on shutdown so CI / sceneviewer runs
    // always leave a trace of where time went, without requiring a key press.
    ::wallpaper::profiler::DumpToStderr();
#endif
}

bool SceneWallpaper::inited() const { return m_main_handler->inited(); }

bool SceneWallpaper::init() { return m_main_handler->init(); }

void SceneWallpaper::initVulkan(const RenderInitInfo& info) {
    m_offscreen                             = info.offscreen;
    std::shared_ptr<RenderInitInfo> sp_info = std::make_shared<RenderInitInfo>(info);
    auto                            msg =
        CreateMsgWithCmd(m_main_handler->renderHandler(), RenderHandler::CMD::CMD_INIT_VULKAN);
    msg->setObject("info", sp_info);
    msg->post();
}

void SceneWallpaper::play() {
    auto msg = CreateMsgWithCmd(m_main_handler, MainHandler::CMD::CMD_STOP);
    msg->setBool("value", false);
    msg->post();
}
void SceneWallpaper::pause() {
    auto msg = CreateMsgWithCmd(m_main_handler, MainHandler::CMD::CMD_STOP);
    msg->setBool("value", true);
    msg->post();
}

void SceneWallpaper::mouseInput(double x, double y) {
    m_main_handler->renderHandler()->setMousePos(x, y);
}

void SceneWallpaper::updateText(int32_t id, const std::string& text) {
    m_main_handler->renderHandler()->setTextUpdate(id, text);
}

void SceneWallpaper::updateTextPointsize(int32_t id, float pointsize) {
    m_main_handler->renderHandler()->setTextPointsize(id, pointsize);
}

void SceneWallpaper::requestPassDump(const std::string& dir) {
    m_main_handler->renderHandler()->requestPassDump(dir);
}
bool SceneWallpaper::passDumpDone() const {
    return m_main_handler->renderHandler()->passDumpDone();
}

void SceneWallpaper::updateColor(int32_t id, float r, float g, float b) {
    m_main_handler->renderHandler()->setColorUpdate(id, r, g, b);
}

std::vector<TextScriptInfo> SceneWallpaper::getTextScripts() const {
    return m_main_handler->getTextScripts();
}

std::vector<ColorScriptInfo> SceneWallpaper::getColorScripts() const {
    return m_main_handler->getColorScripts();
}

std::vector<ShaderValueScriptInfo> SceneWallpaper::getShaderValueScripts() const {
    return m_main_handler->getShaderValueScripts();
}

std::vector<PropertyScriptInfo> SceneWallpaper::getPropertyScripts() const {
    return m_main_handler->getPropertyScripts();
}

std::unordered_map<std::string, int32_t> SceneWallpaper::getNodeNameToIdMap() const {
    return m_main_handler->getNodeNameToIdMap();
}

std::string SceneWallpaper::getLayerInitialStatesJson() const {
    return m_main_handler->getLayerInitialStatesJson();
}

std::array<int32_t, 2> SceneWallpaper::getOrthoSize() const {
    return m_main_handler->getOrthoSize();
}

SceneWallpaper::ParallaxInfo SceneWallpaper::getParallaxInfo() const {
    return m_main_handler->getParallaxInfo();
}

void SceneWallpaper::setHasScriptAudio(bool v) { m_main_handler->setHasScriptAudio(v); }

void SceneWallpaper::updateNodeTransform(int32_t id, const std::string& property, float x, float y,
                                         float z) {
    m_main_handler->renderHandler()->setNodeTransform(id, property, x, y, z);
}

void SceneWallpaper::updateNodeVisible(int32_t id, bool visible) {
    m_main_handler->renderHandler()->setNodeVisible(id, visible);
}

void SceneWallpaper::updateNodeAlpha(int32_t id, float alpha) {
    m_main_handler->renderHandler()->setNodeAlpha(id, alpha);
}

void SceneWallpaper::updateParticleRate(int32_t id, float rate) {
    m_main_handler->renderHandler()->setParticleRate(id, rate);
}

void SceneWallpaper::updateEffectVisible(int32_t nodeId, int32_t effectIndex, bool visible) {
    m_main_handler->renderHandler()->setEffectVisible(nodeId, effectIndex, visible);
}

void SceneWallpaper::updateMaterialValue(int32_t            nodeId,
                                         std::string        name,
                                         std::vector<float> floats) {
    m_main_handler->renderHandler()->setMaterialValue(
        nodeId, std::move(name), std::move(floats));
}

void SceneWallpaper::updateEffectMaterialValue(int32_t            nodeId,
                                               int32_t            effectIdx,
                                               std::string        name,
                                               std::vector<float> floats) {
    m_main_handler->renderHandler()->setEffectMaterialValue(
        nodeId, effectIdx, std::move(name), std::move(floats));
}

void SceneWallpaper::setLayerSpriteFrame(int32_t nodeId, bool wantsManual, int32_t frameIdx) {
    m_main_handler->renderHandler()->setLayerSpriteFrame(nodeId, wantsManual, frameIdx);
}

void SceneWallpaper::updateTextStyle(int32_t     nodeId,
                                     std::string halign,
                                     std::string valign,
                                     std::string fontName) {
    m_main_handler->renderHandler()->setTextStyle(
        nodeId, std::move(halign), std::move(valign), std::move(fontName));
}

std::array<float, 16> SceneWallpaper::getLayerWorldMatrix(int32_t nodeId) const {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    return rh->getLayerWorldMatrix(nodeId);
}

int32_t SceneWallpaper::getLayerBoneIndex(int32_t            nodeId,
                                          const std::string& boneName) const {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return 0;
    return rh->getLayerBoneIndex(nodeId, boneName);
}

Scene::NodeSpriteSnapshot SceneWallpaper::getLayerSpriteSnapshot(int32_t nodeId) const {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return {};
    return rh->getLayerSpriteSnapshot(nodeId);
}

void SceneWallpaper::queueParentChange(int32_t childId, int32_t parentId) {
    m_main_handler->renderHandler()->queueParentChange(childId, parentId);
}

void SceneWallpaper::queueChildSort(int32_t childId, int32_t targetIndex) {
    m_main_handler->renderHandler()->queueChildSort(childId, targetIndex);
}

void SceneWallpaper::applyLayerBatch(const std::vector<LayerBatchUpdate>& batch) {
    m_main_handler->renderHandler()->applyLayerBatch(batch);
}

std::vector<SoundVolumeScriptInfo> SceneWallpaper::getSoundVolumeScripts() const {
    return m_main_handler->getSoundVolumeScripts();
}

void SceneWallpaper::updateSoundVolume(int32_t index, float volume) {
    m_main_handler->updateSoundVolume(index, volume);
}

std::string SceneWallpaper::getUserPropertiesJson() const {
    return m_main_handler->getUserPropertiesJson();
}

double SceneWallpaper::getSceneTime() const {
    auto rh = m_main_handler->renderHandler();
    return rh ? rh->getSceneTime() : 0.0;
}

double SceneWallpaper::getFps() const {
    auto rh = m_main_handler->renderHandler();
    return rh ? (double)rh->fps_counter.Fps() : 0.0;
}

uint64_t SceneWallpaper::getFrameIdx() const {
    auto rh = m_main_handler->renderHandler();
    return rh ? rh->getFrameIdx() : 0ULL;
}

std::vector<int32_t> SceneWallpaper::getVideoTextureNodeIds() const {
    auto rh = m_main_handler->renderHandler();
    return rh ? rh->getVideoNodeIds() : std::vector<int32_t> {};
}
double SceneWallpaper::videoGetCurrentTime(int32_t nodeId) const {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return 0.0;
    auto d = rh->findVideoDecoder(nodeId);
    return d ? d->getCurrentTimeSec() : 0.0;
}
double SceneWallpaper::videoGetDuration(int32_t nodeId) const {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return 0.0;
    auto d = rh->findVideoDecoder(nodeId);
    return d ? d->getDurationSec() : 0.0;
}
bool SceneWallpaper::videoIsPlaying(int32_t nodeId) const {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return false;
    auto d = rh->findVideoDecoder(nodeId);
    return d ? d->isPlaying() : false;
}
void SceneWallpaper::videoPlay(int32_t nodeId) {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return;
    if (auto d = rh->findVideoDecoder(nodeId)) d->play();
}
void SceneWallpaper::videoPause(int32_t nodeId) {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return;
    if (auto d = rh->findVideoDecoder(nodeId)) d->pause();
}
void SceneWallpaper::videoStop(int32_t nodeId) {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return;
    if (auto d = rh->findVideoDecoder(nodeId)) d->stop();
}
void SceneWallpaper::videoSetCurrentTime(int32_t nodeId, double t) {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return;
    if (auto d = rh->findVideoDecoder(nodeId)) d->setCurrentTimeSec(t);
}
void SceneWallpaper::videoSetRate(int32_t nodeId, double rate) {
    auto rh = m_main_handler->renderHandler();
    if (! rh) return;
    if (auto d = rh->findVideoDecoder(nodeId)) d->setRate(rate);
}

std::vector<SoundLayerControlInfo> SceneWallpaper::getSoundLayerControls() const {
    return m_main_handler->getSoundLayerControls();
}
void SceneWallpaper::soundLayerPlay(int32_t index) { m_main_handler->soundLayerPlay(index); }
void SceneWallpaper::soundLayerStop(int32_t index) { m_main_handler->soundLayerStop(index); }
void SceneWallpaper::soundLayerPause(int32_t index) { m_main_handler->soundLayerPause(index); }
bool SceneWallpaper::soundLayerIsPlaying(int32_t index) const {
    return m_main_handler->soundLayerIsPlaying(index);
}
void SceneWallpaper::soundLayerSetVolume(int32_t index, float volume) {
    m_main_handler->soundLayerSetVolume(index, volume);
}

std::vector<AnimationEventInfo> SceneWallpaper::drainAnimationEvents() {
    return m_main_handler->drainAnimationEvents();
}

void SceneWallpaper::propertyAnimPlay(int32_t nodeId, const std::string& name) {
    m_main_handler->renderHandler()->propertyAnimCommand(nodeId, name, "play");
}
void SceneWallpaper::propertyAnimPause(int32_t nodeId, const std::string& name) {
    m_main_handler->renderHandler()->propertyAnimCommand(nodeId, name, "pause");
}
void SceneWallpaper::propertyAnimStop(int32_t nodeId, const std::string& name) {
    m_main_handler->renderHandler()->propertyAnimCommand(nodeId, name, "stop");
}
bool SceneWallpaper::propertyAnimIsPlaying(int32_t nodeId, const std::string& name) const {
    return m_main_handler->renderHandler()->propertyAnimIsPlaying(nodeId, name);
}

// Scene property control forwarding
void SceneWallpaper::updateClearColor(float r, float g, float b) {
    m_main_handler->renderHandler()->setClearColor(r, g, b);
}
void SceneWallpaper::updateBloomStrength(float v) {
    m_main_handler->renderHandler()->setBloomStrength(v);
}
void SceneWallpaper::updateBloomThreshold(float v) {
    m_main_handler->renderHandler()->setBloomThreshold(v);
}
void SceneWallpaper::updateCameraFov(float v) { m_main_handler->renderHandler()->setCameraFov(v); }
void SceneWallpaper::updateCameraLookAt(float ex, float ey, float ez, float cx, float cy, float cz,
                                        float ux, float uy, float uz) {
    m_main_handler->renderHandler()->setCameraLookAt(ex, ey, ez, cx, cy, cz, ux, uy, uz);
}
void SceneWallpaper::updateAmbientColor(float r, float g, float b) {
    m_main_handler->renderHandler()->setAmbientColor(r, g, b);
}
void SceneWallpaper::updateSkylightColor(float r, float g, float b) {
    m_main_handler->renderHandler()->setSkylightColor(r, g, b);
}
void SceneWallpaper::updateLightColor(int32_t index, float r, float g, float b) {
    m_main_handler->renderHandler()->setLightColor(index, r, g, b);
}
void SceneWallpaper::updateLightRadius(int32_t index, float v) {
    m_main_handler->renderHandler()->setLightRadius(index, v);
}
void SceneWallpaper::updateLightIntensity(int32_t index, float v) {
    m_main_handler->renderHandler()->setLightIntensity(index, v);
}
void SceneWallpaper::updateLightPosition(int32_t index, float x, float y, float z) {
    m_main_handler->renderHandler()->setLightPosition(index, x, y, z);
}
std::string SceneWallpaper::getSceneInitialStateJson() const {
    return m_main_handler->getSceneInitialStateJson();
}

#define BASIC_TYPE(NAME, TYPENAME)                                                       \
    void SceneWallpaper::setProperty##NAME(std::string_view name, TYPENAME value) {      \
        auto msg = CreateMsgWithCmd(m_main_handler, MainHandler::CMD::CMD_SET_PROPERTY); \
        msg->setString("property", std::string(name));                                   \
        msg->set##NAME("value", value);                                                  \
        msg->post();                                                                     \
    }

BASIC_TYPE(Bool, bool);
BASIC_TYPE(Int32, int32_t);
BASIC_TYPE(Float, float);
BASIC_TYPE(String, std::string);
BASIC_TYPE(Object, std::shared_ptr<void>);

ExSwapchain* SceneWallpaper::exSwapchain() const {
    return m_main_handler->renderHandler()->exSwapchain();
}

std::shared_ptr<audio::AudioAnalyzer> SceneWallpaper::audioAnalyzer() const {
    return m_main_handler->audioAnalyzer();
}

void SceneWallpaper::requestScreenshot(const std::string& path) {
    m_main_handler->renderHandler()->requestScreenshot(path);
}

void SceneWallpaper::requestScreenshotAtFrame(const std::string& path, uint64_t frame) {
    m_main_handler->renderHandler()->requestScreenshotAtFrame(path, frame);
}

bool SceneWallpaper::screenshotDone() const {
    return m_main_handler->renderHandler()->screenshotDone();
}

void SceneWallpaper::setHidePattern(const std::string& pat) {
    m_main_handler->setHidePattern(pat);
}

MHANDLER_CMD_IMPL(MainHandler, LOAD_SCENE) {
    if (m_render_handler->renderInited()) {
        loadScene();
    }
}

MHANDLER_CMD_IMPL(MainHandler, SET_PROPERTY) {
    std::string property;
    if (msg->findString("property", &property)) {
        if (property == PROPERTY_SOURCE) {
            msg->findString("value", &m_source);
            LOG_INFO("source: %s", m_source.c_str());
            // Keep m_user_props_json — QML may set it before or after source.
            // The scene parser ignores unrecognized property names, so stale
            // props from a previous wallpaper are harmless.  Once the correct
            // USER_PROPS message arrives it will trigger a runtime update or
            // reload if the value differs.
            CALL_MHANDLER_CMD(LOAD_SCENE, msg);
        } else if (property == PROPERTY_ASSETS) {
            msg->findString("value", &m_assets);
            CALL_MHANDLER_CMD(LOAD_SCENE, msg);
        } else if (property == PROPERTY_FPS) {
            int32_t fps { 15 };
            msg->findInt32("value", &fps);
            if (fps >= 5) {
                m_render_handler->frame_timer.SetRequiredFps((uint8_t)fps);
            }
        } else if (property == PROPERTY_FILLMODE) {
            int32_t value;
            if (msg->findInt32("value", &value)) {
                auto nmsg =
                    CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_SET_FILLMODE);
                nmsg->setInt32("value", value);
                nmsg->post();
            }
        } else if (property == PROPERTY_GRAPHIVZ) {
            msg->findBool("value", &m_gen_graphviz);
        } else if (property == PROPERTY_MUTED) {
            bool muted { false };
            msg->findBool("value", &muted);
            m_sound_manager->SetMuted(muted);
        } else if (property == PROPERTY_SYSTEM_AUDIO_CAPTURE) {
            bool enabled { false };
            msg->findBool("value", &enabled);
            if (enabled != m_system_audio_capture) {
                m_system_audio_capture = enabled;
                if (enabled && m_audio_analyzer) {
                    if (! m_audio_capture) {
                        m_audio_capture = std::make_unique<audio::AudioCapture>();
                    }
                    if (m_audio_capture->Init(m_audio_analyzer)) {
                        m_sound_manager->SetAudioAnalyzer(nullptr);
                        LOG_INFO("Audio spectrum: switched to system audio capture");
                    } else {
                        m_audio_capture.reset();
                        m_sound_manager->SetAudioAnalyzer(m_audio_analyzer);
                        LOG_INFO("Audio spectrum: system capture failed, keeping playback tap");
                    }
                } else {
                    m_audio_capture.reset();
                    if (m_audio_analyzer) {
                        m_sound_manager->SetAudioAnalyzer(m_audio_analyzer);
                    }
                    LOG_INFO("Audio spectrum: switched to playback tap (wallpaper BGM)");
                }
            }
        } else if (property == PROPERTY_VOLUME) {
            float volume { 1.0f };
            msg->findFloat("value", &volume);
            m_sound_manager->SetVolume(volume);
        } else if (property == PROPERTY_CACHE_PATH) {
            std::string path;
            msg->findString("value", &path);
            m_cache_path = path;
        } else if (property == PROPERTY_FIRST_FRAME_CALLBACK) {
            std::shared_ptr<FirstFrameCallback> cb;
            msg->findObject("value", &cb);
            m_first_frame_callback = *cb;
        } else if (property == PROPERTY_SPEED) {
            float speed { 1.0f };
            if (msg->findFloat("value", &speed)) {
                auto nmsg = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_SET_SPEED);
                nmsg->setFloat("value", speed);
                nmsg->post();
            }
        } else if (property == PROPERTY_USER_PROPS) {
            std::string json;
            msg->findString("value", &json);
            if (m_user_props_json != json) {
                std::string oldJson = m_user_props_json;
                m_user_props_json   = json;
                if (! json.empty() && ! m_source.empty() && ! m_assets.empty()) {
                    // Try runtime update first (no reload)
                    if (m_scene.load() && applyUserPropsRuntime(json)) {
                        LOG_INFO("Applied user properties at runtime (no reload): %s",
                                 json.c_str());
                    } else {
                        LOG_INFO("Reloading scene to apply user properties: %s", json.c_str());
                        CALL_MHANDLER_CMD(LOAD_SCENE, msg);
                    }
                }
            }
        } else if (property == PROPERTY_HDR_OUTPUT) {
            bool value { false };
            msg->findBool("value", &value);
            if (m_hdr_output != value) {
                m_hdr_output = value;
                LOG_INFO("HDR output: %s", value ? "enabled" : "disabled");
                // HDR toggle requires ExSwapchain recreation → full reinit
                if (m_render_handler->renderInited()) {
                    auto nmsg = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_SET_HDR);
                    nmsg->setBool("value", value);
                    nmsg->post();
                }
            }
        } else if (property == PROPERTY_POSTPROCESSING_OVERRIDE) {
            std::string value;
            msg->findString("value", &value);
            if (m_postprocessing_override != value) {
                m_postprocessing_override = value;
                m_scene_parser.SetPostprocessingOverride(value);
                LOG_INFO("Postprocessing override: '%s'",
                         value.empty() ? "(scene default)" : value.c_str());
                // Bloom pipeline selection happens at scene-load, so a runtime
                // change must reload the scene to take effect.
                if (! m_source.empty() && ! m_assets.empty()) {
                    CALL_MHANDLER_CMD(LOAD_SCENE, msg);
                }
            }
        }
    }
}

MHANDLER_CMD_IMPL(MainHandler, STOP) {
    bool stop { false };
    if (msg->findBool("value", &stop)) {
        if (stop) {
            m_sound_manager->Pause();
        } else {
            m_sound_manager->Play();
        }

        auto msg_r = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_STOP);
        msg_r->setBool("value", stop);
        msg_r->post();
    }
}

MHANDLER_CMD_IMPL(MainHandler, FIRST_FRAME) {
    if (m_first_frame_callback) m_first_frame_callback();
}

bool MainHandler::applyUserPropsRuntime(const std::string& newJson) {
    auto scene_sp = m_scene.load();
    if (! scene_sp) return false;
    auto& scene = *scene_sp;

    // If no bindings were recorded during parsing, fall back to reload
    if (scene.userPropVisBindings.empty() && scene.userPropUniformBindings.empty()) {
        LOG_INFO("No runtime user property bindings, falling back to reload");
        return false;
    }

    // Parse the new properties
    nlohmann::json props;
    try {
        props = nlohmann::json::parse(newJson);
        if (! props.is_object()) return false;
    } catch (...) {
        return false;
    }

    // Apply visibility changes by re-resolving raw JSON with updated user properties.
    // This handles both boolean and combo properties without requiring a scene reload.
    {
        std::lock_guard<std::mutex> lock(m_user_props_mutex);
        // Apply new overrides to our persistent copy
        for (auto it = props.begin(); it != props.end(); ++it) {
            m_user_props_resolved.SetProperty(it.key(), it.value());
        }

        for (auto& [propName, bindings] : scene.userPropVisBindings) {
            if (! props.contains(propName)) continue;

            for (auto& b : bindings) {
                // Re-resolve the raw visible JSON with updated user properties
                try {
                    auto visJson  = nlohmann::json::parse(b.rawVisibleJson);
                    auto resolved = m_user_props_resolved.ResolveValue(visJson);
                    bool visible;
                    if (resolved.is_boolean()) {
                        visible = resolved.get<bool>();
                    } else if (resolved.is_number()) {
                        visible = resolved.get<double>() != 0.0;
                    } else if (resolved.is_string()) {
                        auto s  = resolved.get<std::string>();
                        visible = ! s.empty() && s != "0" && s != "false";
                    } else {
                        visible = b.defaultVisible;
                    }
                    LOG_INFO("Runtime resolve: '%s' raw=%s resolved=%s visible=%d was=%d",
                             propName.c_str(),
                             b.rawVisibleJson.c_str(),
                             resolved.dump().c_str(),
                             (int)visible,
                             (int)b.node->IsVisible());
                    if (b.node->IsVisible() != visible) {
                        b.node->SetVisible(visible);
                        LOG_INFO("Runtime user prop: '%s' -> node visible=%d",
                                 propName.c_str(),
                                 (int)visible);
                    }
                } catch (const std::exception& e) {
                    LOG_INFO(
                        "Runtime user prop resolve error: '%s': %s", propName.c_str(), e.what());
                }
            }
        }
    }

    // Apply uniform value changes
    for (auto& [propName, bindings] : scene.userPropUniformBindings) {
        if (! props.contains(propName)) continue;
        auto& val = props[propName];

        // Convert JSON value to float vector
        std::vector<float> floatVec;
        if (val.is_number()) {
            floatVec.push_back(val.get<float>());
        } else if (val.is_string()) {
            std::istringstream iss(val.get<std::string>());
            float              f;
            while (iss >> f) floatVec.push_back(f);
        } else if (val.is_array()) {
            for (const auto& elem : val) {
                if (elem.is_number()) floatVec.push_back(elem.get<float>());
            }
        }
        if (floatVec.empty()) continue;

        for (auto& b : bindings) {
            b.material->customShader.constValues[b.uniformName] = floatVec;
            b.material->customShader.constValuesDirty           = true;
            LOG_INFO("Runtime user prop: '%s' -> uniform '%s' = [%f...]",
                     propName.c_str(),
                     b.uniformName.c_str(),
                     floatVec[0]);
        }
    }

    // Check text layer pointsize bindings for runtime re-rasterization
    for (auto& tl : scene.textLayers) {
        if (tl.pointsizeUserProp.empty()) continue;
        if (! props.contains(tl.pointsizeUserProp)) continue;
        try {
            float newPointsize = props[tl.pointsizeUserProp].get<float>();
            if (newPointsize > 0 && newPointsize != tl.pointsize) {
                LOG_INFO("Runtime text pointsize: id=%d '%s' %.1f -> %.1f",
                         tl.id,
                         tl.pointsizeUserProp.c_str(),
                         tl.pointsize,
                         newPointsize);
                tl.pointsize      = newPointsize;
                tl.pointsizeDirty = true;
                // Queue re-rasterization with current text
                m_render_handler->setTextUpdate(tl.id, tl.currentText);
            }
        } catch (...) {
        }
    }

    // Always return true when bindings exist — unbound properties (script-driven
    // dragging, hover effects, etc.) don't need a full scene reload.
    return true;
}

void MainHandler::loadScene() {
    WEK_PROFILE_SCOPE("MainHandler::loadScene");
    if (m_source.empty() || m_assets.empty()) return;

    LOG_INFO("loading scene: %s", m_source.c_str());

    if (! m_sound_manager->IsInited()) {
        m_sound_manager->Init();
        m_sound_manager->Play();
    } else {
        m_sound_manager->UnMountAll();
    }

    // Create audio analyzer — use system capture only if enabled, otherwise playback tap
    m_audio_analyzer = std::make_shared<audio::AudioAnalyzer>();
    if (m_system_audio_capture) {
        m_audio_capture = std::make_unique<audio::AudioCapture>();
        if (m_audio_capture->Init(m_audio_analyzer)) {
            LOG_INFO("Audio spectrum: using system audio capture (PipeWire/PulseAudio monitor)");
        } else {
            m_audio_capture.reset();
            m_sound_manager->SetAudioAnalyzer(m_audio_analyzer);
            LOG_INFO("Audio spectrum: system capture failed, falling back to playback tap");
        }
    } else {
        m_sound_manager->SetAudioAnalyzer(m_audio_analyzer);
        LOG_INFO("Audio spectrum: using playback tap (wallpaper BGM only)");
    }

    std::shared_ptr<Scene> scene { nullptr };

    // mount assets dir
    std::unique_ptr<fs::VFS> pVfs = std::make_unique<fs::VFS>();
    auto&                    vfs  = *pVfs;
    if (! vfs.IsMounted("assets")) {
        bool sus = vfs.Mount("/assets", fs::CreatePhysicalFs(m_assets), "assets");
        if (! sus) {
            LOG_ERROR("Mount assets dir failed");
            return;
        }
    }
    // Accept either a wallpaper *file* (<dir>/scene.pkg, <dir>/scene.json) or
    // a bare *directory* (as sceneviewer-script sometimes gets handed).  The
    // two cases yield different pkgDir/pkgPath derivations; keep them
    // separate instead of relying on replace_extension() to DWIM.
    std::filesystem::path src_fs { m_source };
    std::error_code       dir_ec;
    bool source_is_dir = std::filesystem::is_directory(src_fs, dir_ec);

    std::filesystem::path pkgDir_fs;
    std::filesystem::path pkgPath_fs;
    if (source_is_dir) {
        pkgDir_fs  = src_fs;
        pkgPath_fs = src_fs / "scene.pkg";
    } else {
        pkgDir_fs  = src_fs.parent_path();
        pkgPath_fs = src_fs;
        pkgPath_fs.replace_extension("pkg");
    }
    std::string pkgPath  = pkgPath_fs.native();
    std::string pkgDir   = pkgDir_fs.native();
    std::string scene_id = pkgDir_fs.filename().native();

    // load pkgfile.  Most wallpapers ship `scene.pkg`; gifscene-type
    // wallpapers (Aesthetic City 843532366) ship `gifscene.pkg` instead,
    // with project.json's "file" field naming `gifscene.json` (inside
    // the pkg, not at the dir root).  Try the project.json-named .pkg
    // before falling back to a physical-dir mount.
    bool pkg_mounted = vfs.Mount("/assets", fs::WPPkgFs::CreatePkgFs(pkgPath));
    if (! pkg_mounted) {
        LOG_INFO("load pkg file %s failed, trying alternate pkg names",
                 pkgPath.c_str());
        // Read project.json to discover the wallpaper's declared `file`
        // (e.g. "gifscene.json"); use its stem as <stem>.pkg.
        std::filesystem::path projPath = pkgDir_fs / "project.json";
        if (std::filesystem::exists(projPath)) {
            std::ifstream ifs(projPath);
            if (ifs.good()) {
                std::string body { std::istreambuf_iterator<char>(ifs),
                                   std::istreambuf_iterator<char>() };
                try {
                    auto proj = nlohmann::json::parse(body);
                    auto it = proj.find("file");
                    if (it != proj.end() && it->is_string()) {
                        std::filesystem::path f { it->get<std::string>() };
                        f.replace_extension("pkg");
                        std::filesystem::path altPath = pkgDir_fs / f.filename();
                        if (std::filesystem::exists(altPath) &&
                            altPath.native() != pkgPath) {
                            LOG_INFO("trying alternate pkg: %s",
                                     altPath.native().c_str());
                            if (vfs.Mount("/assets",
                                          fs::WPPkgFs::CreatePkgFs(altPath.native()))) {
                                pkg_mounted = true;
                                pkgPath = altPath.native();
                            }
                        }
                    }
                } catch (const nlohmann::json::exception&) {
                    // malformed project.json — drop through to physical-dir fallback
                }
            }
        }
    }
    if (! pkg_mounted) {
        LOG_INFO("falling back to physical dir: %s", pkgDir.c_str());
        if (! vfs.Mount("/assets", fs::CreatePhysicalFs(pkgDir))) {
            LOG_ERROR("can't load pkg directory: %s", pkgDir.c_str());
            return;
        }
    }
    if (! m_cache_path.empty()) {
        if (! vfs.Mount("/cache", fs::CreatePhysicalFs(m_cache_path, true), "cache")) {
            LOG_ERROR("can't load cache folder: %s", m_cache_path.c_str());
        } else {
            LOG_INFO("cache folder: %s", m_cache_path.c_str());
        }
    }

    {
        // Read project.json once.  It drives BOTH the scene-file resolver
        // (authoritative "file" entry) and the user-property defaults below.
        std::string project_json_str;
        {
            std::filesystem::path projPath = pkgDir_fs / "project.json";
            if (std::filesystem::exists(projPath)) {
                std::ifstream ifs(projPath);
                if (ifs.good()) {
                    project_json_str.assign(std::istreambuf_iterator<char>(ifs),
                                            std::istreambuf_iterator<char>());
                }
            }
        }

        // Try each candidate filename until one opens non-empty.  Order is:
        //   <source-stem>.json → project.json's `file` → scene.json.
        auto candidates = wekde::sceneresolver::BuildSceneFileCandidates(
            m_source, source_is_dir, project_json_str);

        std::string       scene_src;
        std::string       scene_src_entry;
        const std::string base { "/assets/" };
        for (const auto& cand : candidates) {
            std::string scenePath = base + cand;
            if (! vfs.Contains(scenePath)) continue;
            auto f = vfs.Open(scenePath);
            if (! f) continue;
            std::string body = f->ReadAllStr();
            if (body.empty()) continue;
            scene_src       = std::move(body);
            scene_src_entry = cand;
            break;
        }

        if (scene_src.empty()) {
            LOG_ERROR("Not supported scene type (no scene JSON found under %s)", pkgDir.c_str());
            for (const auto& c : candidates) {
                LOG_ERROR("  tried /assets/%s", c.c_str());
            }
            return;
        }
        LOG_INFO("Loaded scene from /assets/%s", scene_src_entry.c_str());

        // Build user properties once: load defaults from project.json (filesystem,
        // since it lives alongside scene.pkg, not inside it) + apply overrides.
        // Used for both parse-time resolution and runtime re-resolution.
        WPUserProperties userProps;
        {
            if (! project_json_str.empty()) {
                if (userProps.LoadFromProjectJson(project_json_str)) {
                    LOG_INFO("Loaded user property defaults from %s/project.json",
                             pkgDir.c_str());
                }
            }
            if (! m_user_props_json.empty()) {
                LOG_INFO("Applying user properties override: %s", m_user_props_json.c_str());
                userProps.ApplyOverrides(m_user_props_json);
            }
        }

        scene = m_scene_parser.Parse(scene_id, scene_src, vfs, *m_sound_manager, userProps);
        if (! scene) {
            LOG_ERROR("scene parse failed for id=%s — malformed scene.json, aborting load",
                      scene_id.c_str());
            return; // MainHandler::loadScene is void; nothing published yet, safe to bail
        }
        scene->vfs.swap(pVfs);

        // Store for runtime re-resolution of combo visibility
        {
            std::lock_guard<std::mutex> lock(m_user_props_mutex);
            m_user_props_resolved = userProps;
        }
    }
    // Connect audio analyzer to shader value updater
    scene->audioAnalyzer = m_audio_analyzer;
    if (auto* wpUpdater = dynamic_cast<WPShaderValueUpdater*>(scene->shaderValueUpdater.get())) {
        wpUpdater->SetAudioAnalyzer(m_audio_analyzer);
        // push the reactive-particle term so the FFT gate keeps
        // Process() running for audio-reactive-particle scenes.  Set here (load
        // thread, before the scene is published) so it happens-before the first
        // render-thread FrameBegin read.
        wpUpdater->SetHasReactiveParticles(scene->hasAudioReactiveParticles);
        LOG_INFO("Audio analyzer connected to shader value updater");
    }

    m_scene.store(scene); // keep reference for runtime user property updates

    // Write active user property bindings to disk for the config UI.
    // Include ALL project.json properties — some are used only by property
    // scripts (e.g. via applyUserProperties / shared vars) and have no
    // direct visibility or uniform bindings in scene.json.
    {
        std::set<std::string> activeProps;
        {
            std::lock_guard<std::mutex> lock(m_user_props_mutex);
            m_user_props_resolved.InsertAllNames(activeProps);
        }
        for (const auto& [name, _] : scene->userPropUniformBindings) activeProps.insert(name);
        for (const auto& [name, _] : scene->userPropVisBindings) activeProps.insert(name);

        std::string configDir;
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0]) {
            configDir = std::string(xdg) + "/wekde/wallpaper";
        } else if (const char* home = std::getenv("HOME"); home && home[0]) {
            configDir = std::string(home) + "/.config/wekde/wallpaper";
        }
        if (! configDir.empty()) {
            std::filesystem::create_directories(configDir);
            nlohmann::json j           = activeProps;
            std::string   bindingsPath = configDir + "/" + std::string(scene_id) + "_bindings.json";
            std::ofstream ofs(bindingsPath);
            if (ofs) {
                ofs << j.dump();
                LOG_INFO(
                    "Wrote %zu active bindings to %s", activeProps.size(), bindingsPath.c_str());
            }
        }
    }

    // Extract text scripts for QML-side evaluation
    {
        std::lock_guard<std::mutex> lock(m_text_scripts_mutex);
        m_text_scripts.clear();
        for (const auto& tl : scene->textLayers) {
            TextScriptInfo tsi;
            tsi.id               = tl.id;
            tsi.script           = tl.script;
            tsi.scriptProperties = tl.scriptProperties;
            tsi.initialValue     = tl.currentText;
            m_text_scripts.push_back(std::move(tsi));
        }
        if (! m_text_scripts.empty()) {
            LOG_INFO("loadScene: %zu text layers with scripts", m_text_scripts.size());
        }
    }

    // Extract color scripts for QML-side evaluation
    {
        std::lock_guard<std::mutex> lock(m_color_scripts_mutex);
        m_color_scripts.clear();
        for (const auto& cs : scene->colorScripts) {
            ColorScriptInfo csi;
            csi.id               = cs.id;
            csi.script           = cs.script;
            csi.scriptProperties = cs.scriptProperties;
            csi.initialColor     = cs.initialColor;
            m_color_scripts.push_back(std::move(csi));
        }
        if (! m_color_scripts.empty()) {
            LOG_INFO("loadScene: %zu color scripts", m_color_scripts.size());
        }
    }

    // Extract shader-value scripts.
    {
        std::lock_guard<std::mutex> lock(m_shader_value_scripts_mutex);
        m_shader_value_scripts.clear();
        for (const auto& sv : scene->shaderValueScripts) {
            ShaderValueScriptInfo svi;
            svi.id               = sv.id;
            svi.effectIdx        = sv.effectIdx;
            svi.uniformName      = sv.uniformName;
            svi.script           = sv.script;
            svi.scriptProperties = sv.scriptProperties;
            svi.initialValue     = sv.initialValue;
            svi.argShape         = sv.argShape;
            m_shader_value_scripts.push_back(std::move(svi));
        }
        if (! m_shader_value_scripts.empty()) {
            LOG_INFO("loadScene: %zu shader-value scripts", m_shader_value_scripts.size());
        }
    }

    // Extract property scripts for QML-side evaluation
    {
        std::lock_guard<std::mutex> lock(m_property_scripts_mutex);
        m_property_scripts.clear();
        for (const auto& ps : scene->propertyScripts) {
            PropertyScriptInfo psi;
            psi.id               = ps.id;
            psi.property         = ps.property;
            psi.script           = ps.script;
            psi.scriptProperties = ps.scriptProperties;
            psi.layerName        = ps.layerName;
            psi.initialVisible   = ps.initialVisible;
            psi.initialVec3      = ps.initialVec3;
            psi.initialFloat     = ps.initialFloat;
            psi.attachment =
                (ps.attachment == wallpaper::ScenePropertyScript::Attachment::AnimationLayer)
                    ? PropertyScriptInfo::Attachment::AnimationLayer
                    : PropertyScriptInfo::Attachment::Object;
            psi.animationLayerIndex = ps.animationLayerIndex;
            m_property_scripts.push_back(std::move(psi));
        }
        if (! m_property_scripts.empty()) {
            LOG_INFO("loadScene: %zu property scripts", m_property_scripts.size());
        }
    }

    // Extract sound volume scripts for QML-side evaluation
    {
        std::lock_guard<std::mutex> lock(m_sound_volume_scripts_mutex);
        m_sound_volume_scripts.clear();
        m_sound_volume_streams.clear();
        for (int32_t i = 0; i < (int32_t)scene->soundVolumeScripts.size(); i++) {
            const auto&           svs = scene->soundVolumeScripts[i];
            SoundVolumeScriptInfo info;
            info.index            = i;
            info.script           = svs.script;
            info.scriptProperties = svs.scriptProperties;
            info.layerName        = svs.layerName;
            info.initialVolume    = svs.initialVolume;
            info.hasAnimation     = svs.hasAnimation;
            if (svs.hasAnimation) {
                info.animation.name   = svs.animation.name;
                info.animation.mode   = svs.animation.mode;
                info.animation.fps    = svs.animation.fps;
                info.animation.length = svs.animation.length;
                for (const auto& kf : svs.animation.keyframes)
                    info.animation.keyframes.push_back({ kf.frame, kf.value });
            }
            m_sound_volume_scripts.push_back(std::move(info));
            m_sound_volume_streams.push_back(svs.streamPtr);
        }
        if (! m_sound_volume_scripts.empty()) {
            LOG_INFO("loadScene: %zu sound volume scripts", m_sound_volume_scripts.size());
        }
    }

    // Extract sound layers for SceneScript play/stop/pause API
    {
        std::lock_guard<std::mutex> lock(m_sound_layers_mutex);
        m_sound_layer_controls.clear();
        m_sound_layer_streams.clear();
        for (const auto& sl : scene->soundLayers) {
            SoundLayerControlInfo info;
            info.name          = sl.name;
            info.initialVolume = sl.initialVolume;
            info.startsilent   = sl.startsilent;
            m_sound_layer_controls.push_back(std::move(info));
            m_sound_layer_streams.push_back(sl.streamPtr);
        }
        if (! m_sound_layer_controls.empty()) {
            LOG_INFO("loadScene: %zu sound layers for SceneScript API",
                     m_sound_layer_controls.size());
        }
    }

    // Store layer name → node ID mapping for thisScene.getLayer()
    {
        std::lock_guard<std::mutex> lock(m_name_map_mutex);
        m_node_name_to_id = scene->nodeNameToId;
        if (! m_node_name_to_id.empty()) {
            LOG_INFO("loadScene: %zu named layers for thisScene.getLayer()",
                     m_node_name_to_id.size());
        }
    }

    // Serialize layer initial states as JSON for JS proxy initialization.
    // Includes parent name (`pn`) and node id (`id`) for SceneScript
    // hierarchy methods — see Scene::SerializeLayerInitialStates.
    {
        std::lock_guard<std::mutex> lock(m_layer_init_mutex);
        m_layer_init_json = scene->SerializeLayerInitialStates();
    }

    // Serialize scene-level initial state for JS thisScene properties
    {
        std::lock_guard<std::mutex> lock(m_scene_init_mutex);
        nlohmann::json              j;
        j["cc"]    = { scene->clearColor[0], scene->clearColor[1], scene->clearColor[2] };
        j["bloom"] = scene->bloomConfig.enabled;
        j["bs"]    = scene->bloomConfig.strength;
        j["bt"]    = scene->bloomConfig.threshold;
        j["ac"]    = { scene->ambientColor[0], scene->ambientColor[1], scene->ambientColor[2] };
        j["sc"]    = { scene->skylightColor[0], scene->skylightColor[1], scene->skylightColor[2] };

        bool persp = scene->activeCamera && scene->activeCamera->IsPerspective();
        j["persp"] = persp;
        if (persp) {
            auto eye = scene->activeCamera->GetEye();
            auto ctr = scene->activeCamera->GetCenter();
            auto up  = scene->activeCamera->GetUp();
            j["fov"] = scene->activeCamera->Fov();
            j["eye"] = { eye.x(), eye.y(), eye.z() };
            j["ctr"] = { ctr.x(), ctr.y(), ctr.z() };
            j["up"]  = { up.x(), up.y(), up.z() };
        } else {
            j["fov"] = 0;
            j["eye"] = { 0, 0, 1 };
            j["ctr"] = { 0, 0, 0 };
            j["up"]  = { 0, 1, 0 };
        }

        nlohmann::json lightsArr = nlohmann::json::array();
        for (auto& l : scene->lights) {
            auto c = l->color();
            auto p = l->node() ? l->node()->Translate() : Eigen::Vector3f::Zero();
            lightsArr.push_back({ { "c", { c.x(), c.y(), c.z() } },
                                  { "r", l->radius() },
                                  { "i", l->intensity() },
                                  { "p", { p.x(), p.y(), p.z() } } });
        }
        j["lights"]       = lightsArr;
        m_scene_init_json = j.dump();
    }

    {
        auto msg = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_SET_SCENE);
        msg->setObject("scene", scene);
        msg->post();
    }

    // draw first frame
    {
        auto msg = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_DRAW);
        msg->post();
    }
}
void MainHandler::sendCmdLoadScene() {
    auto msg = CreateMsgWithCmd(shared_from_this(), MainHandler::CMD::CMD_LOAD_SCENE);
    msg->post();
}
void MainHandler::sendFirstFrameOk() {
    auto msg = CreateMsgWithCmd(shared_from_this(), MainHandler::CMD::CMD_FIRST_FRAME);
    msg->post();
}

bool MainHandler::init() {
    if (m_inited) return true;
    m_main_loop->setName("main");
    m_render_loop->setName("render");

    m_main_loop->start();
    m_render_loop->start();

    m_main_loop->registerHandler(shared_from_this());
    m_render_loop->registerHandler(m_render_handler);

    {
        auto  msg        = CreateMsgWithCmd(m_render_handler, RenderHandler::CMD::CMD_DRAW);
        auto& frameTimer = m_render_handler->frame_timer;
        frameTimer.SetCallback([msg]() {
            msg->post();
        });
        frameTimer.SetRequiredFps(15);
        frameTimer.Run();
    }

    m_inited = true;
    return true;
}
MainHandler::MainHandler()
    : m_sound_manager(std::make_unique<audio::SoundManager>()),
      m_audio_analyzer(std::make_shared<audio::AudioAnalyzer>()),
      m_main_loop(std::make_shared<looper::Looper>()),
      m_render_loop(std::make_shared<looper::Looper>()),
      m_render_handler(std::make_shared<RenderHandler>(*this)) {}
