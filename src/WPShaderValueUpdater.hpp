#pragma once
#include <memory>
#include <mutex>
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>
#include <chrono>

#include <Eigen/Dense>

#include "Core/Core.hpp"
#include "Interface/IShaderValueUpdater.h"
#include "Core/MapSet.hpp"
#include "SpriteAnimation.hpp"
#include "WPPuppet.hpp"
#include "Audio/AudioAnalyzer.h"

namespace wallpaper
{

class Scene;

struct WPUniformInfo {
    bool has_MI { false };
    bool has_M { false };
    bool has_AM { false };
    bool has_MVP { false };
    bool has_MVPI { false };
    bool has_ETVP { false };
    bool has_ETVPI { false };
    bool has_VP { false };

    bool has_BONES { false };
    bool has_TIME { false };
    bool has_DAYTIME { false };
    bool has_POINTERPOSITION { false };
    bool has_PARALLAXPOSITION { false };
    bool has_TEXELSIZE { false };
    bool has_TEXELSIZEHALF { false };
    bool has_SCREEN { false };
    bool has_LIGHTAMBIENTCOLOR { false };
    bool has_LIGHTSKYLIGHTCOLOR { false };
    bool has_LP { false };
    bool has_LCR { false };
    bool has_EYEPOSITION { false };

    bool has_AUDIOSPECTRUM16LEFT { false };
    bool has_AUDIOSPECTRUM16RIGHT { false };
    bool has_AUDIOSPECTRUM32LEFT { false };
    bool has_AUDIOSPECTRUM32RIGHT { false };
    bool has_AUDIOSPECTRUM64LEFT { false };
    bool has_AUDIOSPECTRUM64RIGHT { false };

    struct Tex {
        bool has_resolution { false };
        bool has_mipmap { false };
    };
    std::array<Tex, 12> texs;
};

struct WPShaderValueData {
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    // index + name
    std::vector<std::pair<usize, std::string>> renderTargets;

    WPPuppetLayer puppet_layer;
};

struct WPCameraParallax {
    bool  enable { false };
    float amount { 1.0f };
    float delay { 0.5f };
    float mouseinfluence { 1.0f };
};

struct WPCameraShake {
    bool  enable { false };
    float amplitude { 0.5f };
    float speed { 1.0f };
    float roughness { 0.5f };
};

// Puppet animation event fired by the render thread. Drained by the QML
// thread each tick and forwarded to the owning node's SceneScript handler.
struct PendingAnimationEvent {
    i32         nodeId { -1 };
    i32         frame { 0 };
    std::string name;
};

class WPShaderValueUpdater : public IShaderValueUpdater {
public:
    WPShaderValueUpdater(Scene* scene): m_scene(scene) {}
    virtual ~WPShaderValueUpdater() {}

    void FrameBegin() override;

    void InitUniforms(SceneNode*, const ExistsUniformOp&) override;
    void UpdateUniforms(SceneNode*, sprite_map_t&, const UpdateUniformOp&) override;
    void UpdateUniforms(SceneNode*, sprite_map_t&, const UpdateUniformOp&,
                        const std::string& camera_override) override;
    void FrameEnd() override;
    void MouseInput(double, double) override;
    void SetTexelSize(float x, float y) override;

    void                    SetNodeData(void*, const WPShaderValueData&);
    void                    SetCameraParallax(const WPCameraParallax& value) { m_parallax = value; }
    const WPCameraParallax& GetCameraParallax() const { return m_parallax; }
    void                    SetCameraShake(const WPCameraShake& value) { m_shake = value; }
    void                    SetAudioAnalyzer(std::shared_ptr<audio::AudioAnalyzer> analyzer) {
        m_audioAnalyzer = std::move(analyzer);
    }

    // Append events fired by a specific node's puppet animation layer during
    // this frame.  Called from the render thread; not thread-safe.
    void PushAnimationEvents(i32 nodeId, std::vector<WPPuppetLayer::PendingEvent> events);

    // Move all pending animation events out (and reset the queue).  Designed
    // to be called from the QML/script thread; holds a mutex to synchronise
    // with render-thread pushes.
    std::vector<PendingAnimationEvent> DrainAnimationEvents();

    void SetScreenSize(i32 w, i32 h) override { m_screen_size = { (float)w, (float)h }; }

    // Get interpolated mouse position in normalized coordinates (0-1)
    std::array<float, 2> GetMousePosition() const { return m_mousePos; }

private:
    Scene*               m_scene;
    WPCameraParallax     m_parallax;
    WPCameraShake        m_shake;
    Eigen::Vector2f      m_shakeOffset { 0.0f, 0.0f };
    double               m_dayTime { 0.0f };
    std::array<float, 2> m_texelSize { 1.0f / 1920.0f, 1.0f / 1080.0f };

    std::array<float, 2> m_mousePos { 0.5f, 0.5f };
    std::array<float, 2> m_mousePosInput { 0.5f, 0.5f };
    double               m_mouseDelayedTime { 0.0f };
    uint                 m_mouseInputCount { 0 };

    std::chrono::time_point<std::chrono::steady_clock> m_last_mouse_input_time;

    std::array<float, 2> m_screen_size { 1920, 1080 };

    std::shared_ptr<audio::AudioAnalyzer> m_audioAnalyzer;

    Map<void*, WPShaderValueData> m_nodeDataMap;
    Map<void*, WPUniformInfo>     m_nodeUniformInfoMap;

    std::mutex                         m_anim_events_mtx;
    std::vector<PendingAnimationEvent> m_anim_events;
};
} // namespace wallpaper
