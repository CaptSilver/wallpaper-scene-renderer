#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"

#include "Utils/Logging.h"
#include "Looper/Looper.hpp"

#include "Timer/FrameTimer.hpp"
#include "Utils/FpsCounter.h"
#include "WPSceneParser.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneImageEffectLayer.h"
#include "Particle/ParticleSystem.h"
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
#include "HWVideoTextureDecoder.hpp"
#endif
#include "Image.hpp"

#include "RenderGraph/RenderGraph.hpp"

#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/VulkanRender.hpp"
#include "WPTextRenderer.hpp"

#include "WPUserProperties.hpp"
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
        if (m_scene) return { m_scene->ortho[0], m_scene->ortho[1] };
        return { 1920, 1080 };
    }

    std::shared_ptr<audio::AudioAnalyzer> audioAnalyzer() const { return m_audio_analyzer; }

    std::vector<AnimationEventInfo> drainAnimationEvents() {
        // m_scene is assigned once at load on the main thread; read from QML
        // thread here.  Matches the lockless pattern used elsewhere
        // (e.g. getOrthoSize()).  The underlying DrainAnimationEvents() is
        // itself mutex-protected against the render-thread writer.
        auto scene = m_scene;
        if (! scene) return {};
        auto* updater = dynamic_cast<WPShaderValueUpdater*>(scene->shaderValueUpdater.get());
        if (! updater) return {};
        auto raw = updater->DrainAnimationEvents();
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

    WPSceneParser                         m_scene_parser;
    std::unique_ptr<audio::SoundManager>  m_sound_manager;
    std::shared_ptr<audio::AudioAnalyzer> m_audio_analyzer;
    std::unique_ptr<audio::AudioCapture>  m_audio_capture;
    FirstFrameCallback                    m_first_frame_callback;
    std::string                           m_user_props_json;
    std::shared_ptr<Scene>                m_scene; // shared with render handler

    mutable std::mutex                       m_text_scripts_mutex;
    std::vector<TextScriptInfo>              m_text_scripts;
    mutable std::mutex                       m_color_scripts_mutex;
    std::vector<ColorScriptInfo>             m_color_scripts;
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
        if (m_scene && m_render->inited()) {
            m_render->clearLastRenderGraph(m_scene.get());
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
    bool screenshotDone() const {
        return m_render && m_render->screenshotDone();
    }

    void requestPassDump(const std::string& dir) {
        if (m_render) m_render->setPassDumpDir(dir);
    }
    bool passDumpDone() const {
        return m_render && m_render->passDumpDone();
    }

    void setMousePos(double x, double y) { m_mouse_pos.store(std::array { (float)x, (float)y }); }

    void setTextUpdate(i32 id, const std::string& text) {
        std::lock_guard<std::mutex> lock(m_text_update_mutex);
        m_pending_text_updates[id] = text;
    }

    void setTextPointsize(i32 id, float pointsize) {
        std::lock_guard<std::mutex> lock(m_text_update_mutex);
        m_pending_pointsize_updates[id] = pointsize;
    }

    void setColorUpdate(i32 id, float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_color_update_mutex);
        m_pending_color_updates[id] = { r, g, b };
        LOG_INFO("setColorUpdate enqueued id=%d rgb=(%.3f,%.3f,%.3f)", id, r, g, b);
    }

    void setNodeTransform(i32 id, const std::string& property, float x, float y, float z) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_transform_updates[{ id, property }] = { x, y, z };
        static int s_transform_log                    = 0;
        if (++s_transform_log <= 5 || s_transform_log % 1000 == 0) {
            LOG_INFO("setNodeTransform[%d]: id=%d prop=%s val=(%.4f,%.4f,%.4f)",
                     s_transform_log,
                     id,
                     property.c_str(),
                     x,
                     y,
                     z);
        }
    }

    void setNodeVisible(i32 id, bool visible) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_visible_updates[id] = visible;
        static int s_visible_log      = 0;
        if (++s_visible_log <= 5 || s_visible_log % 1000 == 0) {
            LOG_INFO("setNodeVisible[%d]: id=%d visible=%d", s_visible_log, id, (int)visible);
        }
    }

    void setEffectVisible(i32 nodeId, i32 effectIndex, bool visible) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_effect_visible.emplace_back(nodeId, effectIndex, visible);
    }

    void setNodeAlpha(i32 id, float alpha) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_alpha_updates[id] = alpha;
        static int s_alpha_log      = 0;
        if (++s_alpha_log <= 5 || s_alpha_log % 1000 == 0) {
            LOG_INFO("setNodeAlpha[%d]: id=%d alpha=%.4f", s_alpha_log, id, alpha);
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
    void setCameraLookAt(float ex, float ey, float ez,
                         float cx, float cy, float cz,
                         float ux, float uy, float uz) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_camera_lookat = CameraLookAtUpdate {
            { (double)ex, (double)ey, (double)ez },
            { (double)cx, (double)cy, (double)cz },
            { (double)ux, (double)uy, (double)uz }
        };
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
        // If device was lost, attempt recovery
        if (m_render->deviceLost()) {
            recoverFromDeviceLost();
            return;
        }

        frame_timer.FrameBegin();
        if (m_rg) {
            // LOG_INFO("frame info, fps: %.1f, frametime: %.1f", 1.0f, 1000.0f*m_scene->frameTime);
            m_scene->shaderValueUpdater->FrameBegin();
            {
                auto pos = m_mouse_pos.load();
                m_scene->shaderValueUpdater->MouseInput(pos[0], pos[1]);

                // Update particle control points that follow the mouse
                auto* wpUpdater =
                    static_cast<WPShaderValueUpdater*>(m_scene->shaderValueUpdater.get());
                auto mousePos = wpUpdater->GetMousePosition();
                m_scene->paritileSys->UpdateMouseControlPoints(
                    mousePos, { m_scene->ortho[0], m_scene->ortho[1] });
            }
            m_scene->paritileSys->Emitt();

            // Auto-hide pool particle nodes whose burst has played out.
            // SceneScript pool-particle assets (e.g. dino_run's coinget)
            // fire their instantaneous emitter on first tick after
            // createLayer, particles live their lifetime, then die.  If the
            // script doesn't promptly call destroyLayer, the node stays
            // "visible" with zero active particles.  Hide it here so pool
            // slots release automatically; JS destroyLayer then re-pushes
            // the name to the pool for reuse.
            for (auto& [nodeId, sub] : m_scene->particleSubByNodeId) {
                if (! sub || ! sub->IsBurstDone()) continue;
                auto nit = m_scene->nodeById.find(nodeId);
                if (nit == m_scene->nodeById.end() || ! nit->second) {
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
                    for (auto& tl : m_scene->textLayers) {
                        if (tl.id != id) continue;
                        if (newSize > 0.0f && std::abs(tl.pointsize - newSize) > 0.01f) {
                            tl.pointsize      = newSize;
                            tl.pointsizeDirty = true;
                        }
                        break;
                    }
                }
                for (auto& [id, newText] : m_pending_text_updates) {
                    for (auto& tl : m_scene->textLayers) {
                        if (tl.id != id) continue;
                        // Re-rasterize if text or pointsize changed
                        if (tl.currentText == newText && ! tl.pointsizeDirty) break;
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
                        }
                        break;
                    }
                }
                m_pending_text_updates.clear();
                m_pending_pointsize_updates.clear();
            }

            // Process video texture frame updates — only decode visible videos
            for (auto& vd : m_video_decoders) {
                if (! vd.decoder) continue;
                // Visibility gating: pause invisible, resume visible
                bool visible = (! vd.ownerNode || vd.ownerNode->IsVisible());
                if (! visible) {
                    if (vd.decoder->isPlaying()) vd.decoder->pause();
                    continue;
                }
                if (! vd.decoder->isPlaying()) vd.decoder->play();
                if (! vd.decoder->hasNewFrame()) continue;
                const uint8_t* frameData = vd.decoder->acquireFrame();
                if (! frameData) continue;

                Image img;
                img.key = vd.textureKey;
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
                mip.data = ImageDataPtr(const_cast<uint8_t*>(frameData), [](uint8_t*) {});
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
                             m_scene->colorScripts.size());
                }
                for (auto& [id, rgb] : m_pending_color_updates) {
                    for (auto& cs : m_scene->colorScripts) {
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
                             m_scene->nodeById.size());
                }
                int transformHit = 0, transformMiss = 0;
                int effectRedirects   = 0;
                int sampleCount       = 0;
                int planetSampleCount = 0;
                for (auto& [key, vec] : m_pending_transform_updates) {
                    auto [id, prop] = key;
                    auto nit        = m_scene->nodeById.find(id);
                    if (nit == m_scene->nodeById.end()) {
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
                    auto       eit            = m_scene->nodeEffectLayerMap.find(id);
                    SceneNode* resolvedOutput = nullptr;
                    if (eit != m_scene->nodeEffectLayerMap.end()) {
                        resolvedOutput = eit->second->ResolvedLastOutput();
                        effectRedirects++;
                    }

                    if (prop == "origin") {
                        if (resolvedOutput) {
                            resolvedOutput->SetTranslate(v);
                        } else {
                            node->SetTranslate(v);
                        }
                    } else if (prop == "scale") {
                        if (resolvedOutput) {
                            resolvedOutput->SetScale(v);
                        } else {
                            node->SetScale(v);
                        }
                    } else if (prop == "angles") {
                        // WE SceneScript outputs angles in degrees; Eigen AngleAxis expects radians
                        constexpr float deg2rad = M_PI / 180.0f;
                        Eigen::Vector3f rv(v[0] * deg2rad, v[1] * deg2rad, v[2] * deg2rad);
                        if (resolvedOutput) {
                            resolvedOutput->SetRotation(rv);
                        } else {
                            node->SetRotation(rv);
                        }
                    }
                }
                int visHit = 0, visMiss = 0;
                for (auto& [id, visible] : m_pending_visible_updates) {
                    auto nit = m_scene->nodeById.find(id);
                    if (nit != m_scene->nodeById.end()) {
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
                            auto pit = m_scene->particleSubByNodeId.find(id);
                            if (pit != m_scene->particleSubByNodeId.end() && pit->second) {
                                pit->second->Reset();
                            }
                        }
                        // Diagnostic for dynamic-asset pool nodes
                        if (id >= 2'000'000 && id < 2'000'100) {
                            LOG_INFO("POOL visible apply: id=%d visible=%d translate=(%.1f,%.1f)",
                                     id, (int)visible,
                                     nit->second->Translate().x(),
                                     nit->second->Translate().y());
                        }
                    } else {
                        visMiss++;
                    }
                }
                for (auto& [id, alpha] : m_pending_alpha_updates) {
                    auto nit = m_scene->nodeById.find(id);
                    if (nit == m_scene->nodeById.end()) continue;
                    SceneNode* node = nit->second;
                    if (node->HasMaterial()) {
                        auto* mat                                    = node->Mesh()->Material();
                        mat->customShader.constValues["g_UserAlpha"] = std::vector<float> { alpha };
                        mat->customShader.constValuesDirty           = true;
                    }
                }
                // Apply effect visibility changes
                for (auto& [nodeId, effIdx, vis] : m_pending_effect_visible) {
                    auto eit = m_scene->nodeEffectLayerMap.find(nodeId);
                    if (eit == m_scene->nodeEffectLayerMap.end()) continue;
                    auto* effLayer = eit->second;
                    if (effIdx < 0 || effIdx >= (i32)effLayer->EffectCount()) continue;
                    auto& eff = effLayer->GetEffect(effIdx);
                    eff->visible = vis;
                    for (auto& en : eff->nodes) {
                        en.sceneNode->SetVisible(vis);
                    }
                }
                m_pending_effect_visible.clear();
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
                        auto nit = m_scene->nodeById.find(checkId);
                        if (nit != m_scene->nodeById.end()) {
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
                    m_scene->clearColor = *m_pending_clear_color;
                    m_pending_clear_color.reset();
                }
                if (m_pending_bloom_strength) {
                    m_scene->bloomConfig.strength = *m_pending_bloom_strength;
                    if (! m_scene->bloomConfig.nodes.empty()) {
                        auto* mat = m_scene->bloomConfig.nodes[0]->Mesh()->Material();
                        mat->customShader.constValues["bloomstrength"] =
                            std::vector<float> { *m_pending_bloom_strength };
                        mat->customShader.constValuesDirty = true;
                    }
                    m_pending_bloom_strength.reset();
                }
                if (m_pending_bloom_threshold) {
                    m_scene->bloomConfig.threshold = *m_pending_bloom_threshold;
                    if (! m_scene->bloomConfig.nodes.empty()) {
                        auto* mat = m_scene->bloomConfig.nodes[0]->Mesh()->Material();
                        mat->customShader.constValues["bloomthreshold"] =
                            std::vector<float> { *m_pending_bloom_threshold };
                        mat->customShader.constValuesDirty = true;
                    }
                    m_pending_bloom_threshold.reset();
                }
                if (m_pending_camera_fov && m_scene->activeCamera) {
                    m_scene->activeCamera->SetFov(*m_pending_camera_fov);
                    m_scene->activeCamera->Update();
                    m_pending_camera_fov.reset();
                }
                if (m_pending_camera_lookat && m_scene->activeCamera) {
                    auto& u = *m_pending_camera_lookat;
                    Eigen::Vector3d eye(u.eye[0], u.eye[1], u.eye[2]);
                    Eigen::Vector3d ctr(u.center[0], u.center[1], u.center[2]);
                    Eigen::Vector3d up(u.up[0], u.up[1], u.up[2]);
                    m_scene->activeCamera->SetDirectLookAt(eye, ctr, up);
                    m_pending_camera_lookat.reset();
                }
                if (m_pending_ambient_color) {
                    m_scene->ambientColor = *m_pending_ambient_color;
                    m_pending_ambient_color.reset();
                }
                if (m_pending_skylight_color) {
                    m_scene->skylightColor = *m_pending_skylight_color;
                    m_pending_skylight_color.reset();
                }
                for (auto& u : m_pending_light_colors) {
                    if (u.index >= 0 && u.index < (i32)m_scene->lights.size()) {
                        m_scene->lights[u.index]->setColor(
                            Eigen::Vector3f(u.color[0], u.color[1], u.color[2]));
                    }
                }
                m_pending_light_colors.clear();
                for (auto& u : m_pending_light_radii) {
                    if (u.index >= 0 && u.index < (i32)m_scene->lights.size())
                        m_scene->lights[u.index]->setRadius(u.value);
                }
                m_pending_light_radii.clear();
                for (auto& u : m_pending_light_intensities) {
                    if (u.index >= 0 && u.index < (i32)m_scene->lights.size())
                        m_scene->lights[u.index]->setIntensity(u.value);
                }
                m_pending_light_intensities.clear();
                for (auto& u : m_pending_light_positions) {
                    if (u.index >= 0 && u.index < (i32)m_scene->lights.size()) {
                        auto* node = m_scene->lights[u.index]->node();
                        if (node)
                            node->SetTranslate(
                                Eigen::Vector3f(u.position[0], u.position[1], u.position[2]));
                    }
                }
                m_pending_light_positions.clear();
            }

            // Tick property animations (alpha.animation keyframe tracks).
            // Advances time on playing tracks, evaluates, writes resulting
            // value into the target node's material const (g_UserAlpha for
            // "alpha").  Driven by render-thread frame time so it stays in
            // lockstep with visuals.
            tickPropertyAnimations(frame_timer.IdeaTime() * m_speed);

            m_render->drawFrame(*m_scene);

            // Check for device lost after draw
            if (m_render->deviceLost()) {
                LOG_ERROR(
                    "VK_ERROR_DEVICE_LOST detected during drawFrame, will recover next frame");
                frame_timer.FrameEnd();
                return;
            }

            m_scene->PassFrameTime(frame_timer.IdeaTime() * m_speed);
            m_scene_time.store(m_scene->elapsingTime, std::memory_order_relaxed);

            m_scene->shaderValueUpdater->FrameEnd();
            // fps_counter.RegisterFrame();

            if (! m_scene->first_frame_ok) {
                m_scene->first_frame_ok = true;
                main_handler.sendFirstFrameOk();
            }
        }
        frame_timer.FrameEnd();
    }
    MHANDLER_CMD(SET_FILLMODE) {
        int32_t value;
        if (msg->findInt32("value", &value)) {
            m_fillmode = (FillMode)value;
            if (m_scene && renderInited()) {
                m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
            }
        }
    }
    MHANDLER_CMD(SET_SCENE) {
        if (msg->findObject("scene", &m_scene)) {
            if (m_rg) m_render->clearLastRenderGraph(m_scene.get());
            m_drawDiagReset = true; // force DRAW diagnostic on next frame

            // HDR takes effect only when the renderer pipeline supports it AND the
            // scene's project.json declared hdr:true.  Scenes that set hdr:false
            // expect SDR accumulation (values clamp at 1.0); forcing RGBA16F on
            // those makes overbright materials + additive blending blow out and
            // trip bloom thresholds too aggressively.
            const bool scene_wants_hdr = m_scene->hdrContent;
            const bool effective_hdr   = m_render->hdrContent() && scene_wants_hdr;
            if (effective_hdr) {
                int upgraded = 0;
                for (auto& [name, rt] : m_scene->renderTargets) {
                    if (rt.format == TextureFormat::RGBA8) {
                        rt.format = TextureFormat::RGBA16F;
                        upgraded++;
                    }
                }
                LOG_INFO("HDR content: upgraded %d render targets to RGBA16F", upgraded);
            } else {
                LOG_INFO("HDR content: disabled (scene_hdr=%d, render_hdr=%d)",
                         (int)scene_wants_hdr,
                         (int)m_render->hdrContent());
            }
            m_scene->hdrContent = effective_hdr;
            // Align FinPass tonemap with the effective HDR mode for this scene.
            // If the mode differs from the previous scene, FinPass is marked for
            // re-prepare so it picks the matching tonemap/passthrough shader.
            m_render->setSceneHdrContent(effective_hdr);

            m_rg = sceneToRenderGraph(*m_scene);

            if (main_handler.isGenGraphviz()) m_rg->ToGraphviz("graph.dot");
            m_render->compileRenderGraph(*m_scene, *m_rg);
            m_render->UpdateCameraFillMode(*m_scene, m_fillmode);

            // Create video texture decoders for MP4 textures detected during loading
            // Try HW decoder first (EGL + GL + VA-API), fall back to SW
            m_video_decoders.clear();
            for (auto& vt : m_scene->videoTextures) {
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
                    m_video_decoders.push_back({ vt.textureKey, decoder, vt.ownerNode });
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
            if (m_scene) {
                m_render->clearLastRenderGraph(m_scene.get());
            }
            m_scene.reset();
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
        if (! m_scene) return;
        if (m_scene->nodePropertyAnimations.empty()) return;
        applyPendingPropertyAnimCommands();
        for (auto& [nodeId, anims] : m_scene->nodePropertyAnimations) {
            auto nit = m_scene->nodeById.find(nodeId);
            SceneNode* sourceNode = (nit != m_scene->nodeById.end()) ? nit->second : nullptr;

            for (auto& anim : anims) {
                if (anim.playing) anim.time += dt;
                float value = EvaluatePropertyAnimation(anim, anim.time);
                if (anim.property == "alpha") {
                    writeAlphaToAllMaterials(sourceNode, nodeId, value);
                }
                // Extend here for other properties (color, brightness, …).
            }
        }
    }

    // Apply an alpha value to every material participating in the node's
    // render path so layers with effect chains don't render with a stale
    // baked-in alpha from their per-effect material copy.
    void writeAlphaToAllMaterials(SceneNode* sourceNode, i32 nodeId, float value) {
        auto pushAlpha = [value](SceneMaterial* mat) {
            if (! mat) return;
            mat->customShader.constValues["g_UserAlpha"] =
                std::vector<float> { value };
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
        auto eit = m_scene->nodeEffectLayerMap.find(nodeId);
        if (eit != m_scene->nodeEffectLayerMap.end() && eit->second) {
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

    void propertyAnimCommand(int32_t nodeId, const std::string& name,
                             const std::string& cmd) {
        std::lock_guard<std::mutex> lock(m_prop_anim_cmds_mutex);
        m_prop_anim_cmds.push_back({ nodeId, name, cmd });
    }

    bool propertyAnimIsPlaying(int32_t nodeId, const std::string& name) const {
        if (! m_scene) return false;
        auto it = m_scene->nodePropertyAnimations.find(nodeId);
        if (it == m_scene->nodePropertyAnimations.end()) return false;
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
        for (auto& c : cmds) {
            auto it = m_scene->nodePropertyAnimations.find(c.nodeId);
            if (it == m_scene->nodePropertyAnimations.end()) continue;
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
        if (m_scene) {
            m_render->clearLastRenderGraph(m_scene.get());
        }

        // Clear scene state
        m_scene.reset();
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
    std::shared_ptr<Scene> m_scene { nullptr };
    float                  m_speed { 1.0f };

    std::unique_ptr<vulkan::VulkanRender> m_render;
    std::unique_ptr<rg::RenderGraph>      m_rg { nullptr };

    FillMode m_fillmode { FillMode::ASPECTCROP };

    RenderInitInfo                    m_init_info;
    std::atomic<std::array<float, 2>> m_mouse_pos { std::array { 0.5f, 0.5f } };

    // Published scene clock (m_scene->elapsingTime) — read cross-thread from QML.
    // Writers: this render thread after each PassFrameTime(). Readers: SceneBackend.
    std::atomic<double> m_scene_time { 0.0 };

public:
    double getSceneTime() const { return m_scene_time.load(std::memory_order_relaxed); }
private:

    std::mutex                           m_text_update_mutex;
    std::unordered_map<i32, std::string> m_pending_text_updates;
    std::unordered_map<i32, float>       m_pending_pointsize_updates;

    std::mutex                                    m_color_update_mutex;
    std::unordered_map<i32, std::array<float, 3>> m_pending_color_updates;

    std::mutex                                                  m_property_update_mutex;
    std::map<std::pair<i32, std::string>, std::array<float, 3>> m_pending_transform_updates;
    std::unordered_map<i32, bool>                               m_pending_visible_updates;
    std::unordered_map<i32, float>                              m_pending_alpha_updates;
    std::vector<std::tuple<i32, i32, bool>>                     m_pending_effect_visible; // (nodeId, effectIdx, visible)

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
    struct LightColorUpdate { i32 index; std::array<float, 3> color; };
    struct LightScalarUpdate { i32 index; float value; };
    struct LightPositionUpdate { i32 index; std::array<float, 3> position; };
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
    };
    std::vector<VideoDecoderEntry> m_video_decoders;

    // Queue of script-driven play/stop/pause commands for property animations
    // (layer.getAnimation(name).play() etc.).  Drained at the start of every
    // tickPropertyAnimations() so script intent lands before evaluation.
    struct PropertyAnimCmd { int32_t nodeId; std::string name; std::string cmd; };
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

void SceneWallpaper::updateEffectVisible(int32_t nodeId, int32_t effectIndex, bool visible) {
    m_main_handler->renderHandler()->setEffectVisible(nodeId, effectIndex, visible);
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
void SceneWallpaper::updateCameraFov(float v) {
    m_main_handler->renderHandler()->setCameraFov(v);
}
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

bool SceneWallpaper::screenshotDone() const {
    return m_main_handler->renderHandler()->screenshotDone();
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
                    if (m_scene && applyUserPropsRuntime(json)) {
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
    if (! m_scene) return false;
    auto& scene = *m_scene;

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
    std::filesystem::path pkgPath_fs { m_source };
    pkgPath_fs.replace_extension("pkg");
    std::string pkgPath  = pkgPath_fs.native();
    std::string pkgEntry = pkgPath_fs.filename().replace_extension("json").native();
    std::string pkgDir   = pkgPath_fs.parent_path().native();
    std::string scene_id = pkgPath_fs.parent_path().filename().native();

    // load pkgfile
    if (! vfs.Mount("/assets", fs::WPPkgFs::CreatePkgFs(pkgPath))) {
        LOG_INFO("load pkg file %s failed, fallback to use dir", pkgPath.c_str());
        // load pkg dir
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
        std::string       scene_src;
        const std::string base { "/assets/" };
        {
            std::string scenePath = base + pkgEntry;
            if (vfs.Contains(scenePath)) {
                auto f = vfs.Open(scenePath);
                if (f) scene_src = f->ReadAllStr();
            }
        }
        if (scene_src.empty()) {
            LOG_ERROR("Not supported scene type");
            return;
        }

        // Build user properties once: load defaults from project.json (filesystem,
        // since it lives alongside scene.pkg, not inside it) + apply overrides.
        // Used for both parse-time resolution and runtime re-resolution.
        WPUserProperties userProps;
        {
            std::filesystem::path projPath = std::filesystem::path(pkgDir) / "project.json";
            if (std::filesystem::exists(projPath)) {
                std::ifstream ifs(projPath);
                if (ifs.good()) {
                    std::string content((std::istreambuf_iterator<char>(ifs)),
                                        std::istreambuf_iterator<char>());
                    if (userProps.LoadFromProjectJson(content)) {
                        LOG_INFO("Loaded user property defaults from %s", projPath.c_str());
                    }
                }
            }
            if (! m_user_props_json.empty()) {
                LOG_INFO("Applying user properties override: %s", m_user_props_json.c_str());
                userProps.ApplyOverrides(m_user_props_json);
            }
        }

        scene = m_scene_parser.Parse(scene_id, scene_src, vfs, *m_sound_manager, userProps);
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
        LOG_INFO("Audio analyzer connected to shader value updater");
    }

    m_scene = scene; // keep reference for runtime user property updates

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

    // Serialize layer initial states as JSON for JS proxy initialization
    {
        std::lock_guard<std::mutex> lock(m_layer_init_mutex);
        nlohmann::json              j = nlohmann::json::object();
        for (const auto& [name, lis] : scene->layerInitialStates) {
            auto entry = nlohmann::json{
                { "o", { lis.origin[0], lis.origin[1], lis.origin[2] } },
                { "s", { lis.scale[0], lis.scale[1], lis.scale[2] } },
                { "a", { lis.angles[0], lis.angles[1], lis.angles[2] } },
                { "v", lis.visible },
                { "sz", { lis.size[0], lis.size[1] } } };
            // Include effect names for SceneScript getEffect()
            auto eit = scene->layerEffectNames.find(name);
            if (eit != scene->layerEffectNames.end()) {
                entry["efx"] = eit->second;
            }
            j[name] = std::move(entry);
        }
        j["_ortho"]       = { scene->ortho[0], scene->ortho[1] };
        // Dynamic-asset pools: { "models/coin_0.json": ["__pool_..._0", ...], ... }
        // JS createLayer(asset) pops a pool name, getLayer() turns it into a
        // proxy, and visibility/origin toggling flows through the standard
        // dirty pipeline.
        if (! scene->assetPools.empty()) {
            nlohmann::json pools = nlohmann::json::object();
            for (const auto& [path, names] : scene->assetPools) {
                pools[path] = names;
            }
            j["_assetPools"] = std::move(pools);
        }
        m_layer_init_json = j.dump();
    }

    // Serialize scene-level initial state for JS thisScene properties
    {
        std::lock_guard<std::mutex> lock(m_scene_init_mutex);
        nlohmann::json              j;
        j["cc"]   = { scene->clearColor[0], scene->clearColor[1], scene->clearColor[2] };
        j["bloom"] = scene->bloomConfig.enabled;
        j["bs"]   = scene->bloomConfig.strength;
        j["bt"]   = scene->bloomConfig.threshold;
        j["ac"]   = { scene->ambientColor[0], scene->ambientColor[1], scene->ambientColor[2] };
        j["sc"]   = { scene->skylightColor[0], scene->skylightColor[1], scene->skylightColor[2] };

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
            lightsArr.push_back({
                { "c", { c.x(), c.y(), c.z() } },
                { "r", l->radius() },
                { "i", l->intensity() },
                { "p", { p.x(), p.y(), p.z() } }
            });
        }
        j["lights"] = lightsArr;
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
