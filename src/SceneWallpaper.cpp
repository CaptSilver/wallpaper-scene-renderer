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

#include "RenderGraph/RenderGraph.hpp"

#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/VulkanRender.hpp"
#include "WPTextRenderer.hpp"

#include <nlohmann/json.hpp>
#include <sstream>
#include <atomic>
#include <mutex>
#include <set>
#include <map>
#include <fstream>
#include <filesystem>
#include <unordered_map>

using namespace wallpaper;

#define CASE_CMD(cmd)      \
    case CMD::CMD_##cmd:   \
        handle_##cmd(msg); \
        break;
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

    void updateSoundVolume(int32_t index, float volume) {
        std::lock_guard<std::mutex> lock(m_sound_volume_scripts_mutex);
        if (index >= 0 && index < (int32_t)m_sound_volume_streams.size()) {
            WPSoundParser::SetStreamVolume(m_sound_volume_streams[index], volume);
        }
    }

    std::unordered_map<std::string, int32_t> getNodeNameToIdMap() const {
        std::lock_guard<std::mutex> lock(m_name_map_mutex);
        return m_node_name_to_id;
    }

    std::string getLayerInitialStatesJson() const {
        std::lock_guard<std::mutex> lock(m_layer_init_mutex);
        return m_layer_init_json;
    }

    std::shared_ptr<audio::AudioAnalyzer> audioAnalyzer() const { return m_audio_analyzer; }

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

    WPSceneParser                        m_scene_parser;
    std::unique_ptr<audio::SoundManager> m_sound_manager;
    std::shared_ptr<audio::AudioAnalyzer> m_audio_analyzer;
    std::unique_ptr<audio::AudioCapture> m_audio_capture;
    FirstFrameCallback                   m_first_frame_callback;
    std::string                          m_user_props_json;
    std::shared_ptr<Scene>               m_scene; // shared with render handler

    mutable std::mutex                   m_text_scripts_mutex;
    std::vector<TextScriptInfo>          m_text_scripts;
    mutable std::mutex                   m_color_scripts_mutex;
    std::vector<ColorScriptInfo>         m_color_scripts;
    mutable std::mutex                   m_property_scripts_mutex;
    std::vector<PropertyScriptInfo>      m_property_scripts;
    mutable std::mutex                   m_sound_volume_scripts_mutex;
    std::vector<SoundVolumeScriptInfo>   m_sound_volume_scripts;
    std::vector<void*>                   m_sound_volume_streams; // parallel: WPSoundStream*
    mutable std::mutex                   m_name_map_mutex;
    std::unordered_map<std::string, int32_t> m_node_name_to_id;
    mutable std::mutex                   m_layer_init_mutex;
    std::string                          m_layer_init_json;

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

    void setMousePos(double x, double y) { m_mouse_pos.store(std::array { (float)x, (float)y }); }

    void setTextUpdate(i32 id, const std::string& text) {
        std::lock_guard<std::mutex> lock(m_text_update_mutex);
        m_pending_text_updates[id] = text;
    }

    void setColorUpdate(i32 id, float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_color_update_mutex);
        m_pending_color_updates[id] = { r, g, b };
        LOG_INFO("setColorUpdate enqueued id=%d rgb=(%.3f,%.3f,%.3f)", id, r, g, b);
    }

    void setNodeTransform(i32 id, const std::string& property, float x, float y, float z) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_transform_updates[{id, property}] = { x, y, z };
    }

    void setNodeVisible(i32 id, bool visible) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_visible_updates[id] = visible;
    }

    void setNodeAlpha(i32 id, float alpha) {
        std::lock_guard<std::mutex> lock(m_property_update_mutex);
        m_pending_alpha_updates[id] = alpha;
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

            // Process pending text updates before drawing
            {
                std::lock_guard<std::mutex> lock(m_text_update_mutex);
                for (auto& [id, newText] : m_pending_text_updates) {
                    for (auto& tl : m_scene->textLayers) {
                        if (tl.id == id && tl.currentText != newText) {
                            auto img = WPTextRenderer::RenderText(
                                tl.fontData, tl.pointsize, newText,
                                tl.texWidth, tl.texHeight,
                                tl.halign, tl.valign, tl.padding);
                            if (img) {
                                img->key = tl.textureKey;
                                m_render->reuploadTexture(tl.textureKey, *img);
                                tl.currentText = newText;
                            }
                            break;
                        }
                    }
                }
                m_pending_text_updates.clear();
            }

            // Process pending color updates before drawing
            {
                std::lock_guard<std::mutex> lock(m_color_update_mutex);
                if (!m_pending_color_updates.empty()) {
                    LOG_INFO("DRAW: %zu pending color updates, %zu colorScripts",
                             m_pending_color_updates.size(), m_scene->colorScripts.size());
                }
                for (auto& [id, rgb] : m_pending_color_updates) {
                    for (auto& cs : m_scene->colorScripts) {
                        if (cs.id == id && cs.material) {
                            // Preserve existing alpha from g_Color4
                            float alpha = 1.0f;
                            auto it = cs.material->customShader.constValues.find("g_Color4");
                            if (it != cs.material->customShader.constValues.end() &&
                                it->second.size() >= 4) {
                                alpha = it->second[3];
                            }
                            cs.material->customShader.constValues["g_Color4"] =
                                std::vector<float>{ rgb[0], rgb[1], rgb[2], alpha };
                            cs.material->customShader.constValuesDirty = true;
                            LOG_INFO("color update id=%d: rgb=(%.3f,%.3f,%.3f) alpha=%.3f",
                                     id, rgb[0], rgb[1], rgb[2], alpha);
                            break;
                        }
                    }
                }
                m_pending_color_updates.clear();
            }

            // Process pending property script updates (transform, visibility, alpha)
            {
                std::lock_guard<std::mutex> lock(m_property_update_mutex);
                static int drawDiagCount = 0;
                bool logDiag = (++drawDiagCount % 180 == 1);  // every ~6 sec at 30fps
                if (logDiag) {
                    LOG_INFO("DRAW: pending transforms=%zu visible=%zu alpha=%zu nodeById=%zu",
                             m_pending_transform_updates.size(),
                             m_pending_visible_updates.size(),
                             m_pending_alpha_updates.size(),
                             m_scene->nodeById.size());
                }
                int transformHit = 0, transformMiss = 0;
                int sampleCount = 0;
                int planetSampleCount = 0;
                for (auto& [key, vec] : m_pending_transform_updates) {
                    auto [id, prop] = key;
                    auto nit = m_scene->nodeById.find(id);
                    if (nit == m_scene->nodeById.end()) { transformMiss++; continue; }
                    transformHit++;
                    SceneNode* node = nit->second;
                    Eigen::Vector3f v(vec[0], vec[1], vec[2]);
                    // Sample first 5 transforms + planet-range transforms
                    if (logDiag && sampleCount < 5) {
                        sampleCount++;
                        LOG_INFO("DRAW sample: id=%d prop=%s val=(%.4f, %.4f, %.4f)",
                                 id, prop.c_str(), vec[0], vec[1], vec[2]);
                    }
                    if (logDiag && id >= 1360 && id <= 1400 && planetSampleCount < 10) {
                        planetSampleCount++;
                        LOG_INFO("DRAW planet: id=%d prop=%s val=(%.4f, %.4f, %.4f) visible=%d",
                                 id, prop.c_str(), vec[0], vec[1], vec[2],
                                 (int)node->IsVisible());
                    }
                    if (prop == "origin") {
                        node->SetTranslate(v);
                    } else if (prop == "scale") {
                        node->SetScale(v);
                    } else if (prop == "angles") {
                        node->SetRotation(v);
                    }
                }
                int visHit = 0, visMiss = 0;
                for (auto& [id, visible] : m_pending_visible_updates) {
                    auto nit = m_scene->nodeById.find(id);
                    if (nit != m_scene->nodeById.end()) {
                        visHit++;
                        nit->second->SetVisible(visible);
                    } else {
                        visMiss++;
                    }
                }
                for (auto& [id, alpha] : m_pending_alpha_updates) {
                    auto nit = m_scene->nodeById.find(id);
                    if (nit == m_scene->nodeById.end()) continue;
                    SceneNode* node = nit->second;
                    if (node->HasMaterial()) {
                        auto* mat = node->Mesh()->Material();
                        mat->customShader.constValues["g_UserAlpha"] =
                            std::vector<float>{ alpha };
                        mat->customShader.constValuesDirty = true;
                    }
                }
                if (logDiag) {
                    LOG_INFO("DRAW: transform hit=%d miss=%d, visible hit=%d miss=%d, alpha=%zu",
                             transformHit, transformMiss, visHit, visMiss,
                             m_pending_alpha_updates.size());
                    // Dump world transforms for key planet nodes after applying updates
                    for (int checkId : {1360, 1365, 1373, 1374, 1375, 1376}) {
                        auto nit = m_scene->nodeById.find(checkId);
                        if (nit != m_scene->nodeById.end()) {
                            auto* n = nit->second;
                            n->UpdateTrans();
                            auto wt = n->ModelTrans();
                            auto& t = n->Translate();
                            auto& s = n->Scale();
                            LOG_INFO("NODE %d: trans=(%.3f,%.3f,%.3f) scale=(%.3f,%.3f,%.3f) "
                                     "visible=%d world[0]=(%.4f,%.4f,%.4f,%.4f)",
                                     checkId, t.x(), t.y(), t.z(),
                                     s.x(), s.y(), s.z(),
                                     (int)n->IsVisible(),
                                     wt(0,0), wt(0,1), wt(0,2), wt(0,3));
                        }
                    }
                }
                m_pending_transform_updates.clear();
                m_pending_visible_updates.clear();
                m_pending_alpha_updates.clear();
            }

            m_render->drawFrame(*m_scene);

            // Check for device lost after draw
            if (m_render->deviceLost()) {
                LOG_ERROR("VK_ERROR_DEVICE_LOST detected during drawFrame, will recover next frame");
                frame_timer.FrameEnd();
                return;
            }

            m_scene->PassFrameTime(frame_timer.IdeaTime() * m_speed);

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
            m_drawDiagReset = true;  // force DRAW diagnostic on next frame

            // Upgrade render targets to RGBA16F when HDR content pipeline is active
            if (m_render->hdrContent()) {
                m_scene->hdrContent = true;
                int upgraded = 0;
                for (auto& [name, rt] : m_scene->renderTargets) {
                    if (rt.format == TextureFormat::RGBA8) {
                        rt.format = TextureFormat::RGBA16F;
                        upgraded++;
                    }
                }
                LOG_INFO("HDR content: upgraded %d render targets to RGBA16F", upgraded);
            }

            m_rg = sceneToRenderGraph(*m_scene);

            if (main_handler.isGenGraphviz()) m_rg->ToGraphviz("graph.dot");
            m_render->compileRenderGraph(*m_scene, *m_rg);
            m_render->UpdateCameraFillMode(*m_scene, m_fillmode);
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

    RenderInitInfo m_init_info;
    std::atomic<std::array<float, 2>> m_mouse_pos { std::array { 0.5f, 0.5f } };

    std::mutex                          m_text_update_mutex;
    std::unordered_map<i32, std::string> m_pending_text_updates;

    std::mutex                                        m_color_update_mutex;
    std::unordered_map<i32, std::array<float, 3>>     m_pending_color_updates;

    std::mutex m_property_update_mutex;
    std::map<std::pair<i32, std::string>, std::array<float, 3>> m_pending_transform_updates;
    std::unordered_map<i32, bool>  m_pending_visible_updates;
    std::unordered_map<i32, float> m_pending_alpha_updates;

    bool m_drawDiagReset { false };
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

void SceneWallpaper::updateNodeTransform(int32_t id, const std::string& property,
                                         float x, float y, float z) {
    m_main_handler->renderHandler()->setNodeTransform(id, property, x, y, z);
}

void SceneWallpaper::updateNodeVisible(int32_t id, bool visible) {
    m_main_handler->renderHandler()->setNodeVisible(id, visible);
}

void SceneWallpaper::updateNodeAlpha(int32_t id, float alpha) {
    m_main_handler->renderHandler()->setNodeAlpha(id, alpha);
}

std::vector<SoundVolumeScriptInfo> SceneWallpaper::getSoundVolumeScripts() const {
    return m_main_handler->getSoundVolumeScripts();
}

void SceneWallpaper::updateSoundVolume(int32_t index, float volume) {
    m_main_handler->updateSoundVolume(index, volume);
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
                m_user_props_json = json;
                if (!json.empty() && !m_source.empty() && !m_assets.empty()) {
                    // Try runtime update first (no reload)
                    if (m_scene && applyUserPropsRuntime(json)) {
                        LOG_INFO("Applied user properties at runtime (no reload): %s", json.c_str());
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

    // Apply visibility changes
    for (auto& [propName, bindings] : scene.userPropVisBindings) {
        bool visible;
        if (props.contains(propName)) {
            auto& val = props[propName];
            if (val.is_boolean()) {
                visible = val.get<bool>();
            } else {
                continue;
            }
        } else {
            // Property not in overrides → use default
            visible = bindings.front().defaultVisible;
        }
        for (auto& b : bindings) {
            if (b.node->IsVisible() != visible) {
                b.node->SetVisible(visible);
                LOG_INFO("Runtime user prop: '%s' -> node visible=%d", propName.c_str(), (int)visible);
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
            float f;
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
                     propName.c_str(), b.uniformName.c_str(), floatVec[0]);
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
        scene = m_scene_parser.Parse(scene_id, scene_src, vfs, *m_sound_manager, m_user_props_json);
        scene->vfs.swap(pVfs);
    }
    // Connect audio analyzer to shader value updater
    scene->audioAnalyzer = m_audio_analyzer;
    if (auto* wpUpdater =
            dynamic_cast<WPShaderValueUpdater*>(scene->shaderValueUpdater.get())) {
        wpUpdater->SetAudioAnalyzer(m_audio_analyzer);
        LOG_INFO("Audio analyzer connected to shader value updater");
    }

    m_scene = scene; // keep reference for runtime user property updates

    // Write active user property bindings to disk for the config UI
    {
        std::set<std::string> activeProps;
        for (const auto& [name, _] : scene->userPropUniformBindings)
            activeProps.insert(name);
        for (const auto& [name, _] : scene->userPropVisBindings)
            activeProps.insert(name);

        std::string configDir;
        if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0]) {
            configDir = std::string(xdg) + "/wekde/wallpaper";
        } else if (const char* home = std::getenv("HOME"); home && home[0]) {
            configDir = std::string(home) + "/.config/wekde/wallpaper";
        }
        if (!configDir.empty()) {
            std::filesystem::create_directories(configDir);
            nlohmann::json j = activeProps;
            std::string bindingsPath = configDir + "/" + std::string(scene_id) + "_bindings.json";
            std::ofstream ofs(bindingsPath);
            if (ofs) {
                ofs << j.dump();
                LOG_INFO("Wrote %zu active bindings to %s", activeProps.size(), bindingsPath.c_str());
            }
        }
    }

    // Extract text scripts for QML-side evaluation
    {
        std::lock_guard<std::mutex> lock(m_text_scripts_mutex);
        m_text_scripts.clear();
        for (const auto& tl : scene->textLayers) {
            TextScriptInfo tsi;
            tsi.id           = tl.id;
            tsi.script       = tl.script;
            tsi.initialValue = tl.currentText;
            m_text_scripts.push_back(std::move(tsi));
        }
        if (!m_text_scripts.empty()) {
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
        if (!m_color_scripts.empty()) {
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
        if (!m_property_scripts.empty()) {
            LOG_INFO("loadScene: %zu property scripts", m_property_scripts.size());
        }
    }

    // Extract sound volume scripts for QML-side evaluation
    {
        std::lock_guard<std::mutex> lock(m_sound_volume_scripts_mutex);
        m_sound_volume_scripts.clear();
        m_sound_volume_streams.clear();
        for (int32_t i = 0; i < (int32_t)scene->soundVolumeScripts.size(); i++) {
            const auto& svs = scene->soundVolumeScripts[i];
            SoundVolumeScriptInfo info;
            info.index            = i;
            info.script           = svs.script;
            info.scriptProperties = svs.scriptProperties;
            info.initialVolume    = svs.initialVolume;
            m_sound_volume_scripts.push_back(std::move(info));
            m_sound_volume_streams.push_back(svs.streamPtr);
        }
        if (!m_sound_volume_scripts.empty()) {
            LOG_INFO("loadScene: %zu sound volume scripts", m_sound_volume_scripts.size());
        }
    }

    // Store layer name → node ID mapping for thisScene.getLayer()
    {
        std::lock_guard<std::mutex> lock(m_name_map_mutex);
        m_node_name_to_id = scene->nodeNameToId;
        if (!m_node_name_to_id.empty()) {
            LOG_INFO("loadScene: %zu named layers for thisScene.getLayer()",
                     m_node_name_to_id.size());
        }
    }

    // Serialize layer initial states as JSON for JS proxy initialization
    {
        std::lock_guard<std::mutex> lock(m_layer_init_mutex);
        nlohmann::json j = nlohmann::json::object();
        for (const auto& [name, lis] : scene->layerInitialStates) {
            j[name] = {
                {"o", { lis.origin[0], lis.origin[1], lis.origin[2] }},
                {"s", { lis.scale[0], lis.scale[1], lis.scale[2] }},
                {"a", { lis.angles[0], lis.angles[1], lis.angles[2] }},
                {"v", lis.visible}
            };
        }
        m_layer_init_json = j.dump();
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
