#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>

#include <Eigen/Dense>

#include "Core/Core.hpp"
#include "Interface/IShaderValueUpdater.h"
#include "Core/MapSet.hpp"
#include "Scene/SceneShader.h"      // ShaderValue
#include "Scene/UniformDirtyGate.h" // uniformMatricesShouldRecompute
#include "SpriteAnimation.hpp"
#include "WPPuppet.hpp"

namespace wallpaper
{

namespace audio { class AudioAnalyzer; }

class Scene;

// Wallpaper Engine's g_Time is a float32 "seconds since the wallpaper started".
// The scene clock is an unbounded double; narrowing it straight to float32 means
// the mantissa runs out after hours, so a 1/60s frame step stops being
// representable and time-driven UV shaders (waterwaves, scroll) quantize into
// visible blocks — the "pixelation after running a long time" symptom. Wrap the
// clock to a bounded period before the cast so g_Time keeps sub-millisecond
// resolution no matter how long it runs. The only cost is a one-frame phase seam
// in periodic motion once per period, imperceptible for organic wind/flow; shader
// authors who loop g_Time themselves conventionally do so near ~600s. A period
// that is a multiple of 3600 keeps the seam aligned to a round hour.
inline constexpr double kSceneTimeWrapPeriodSec = 3600.0;
inline float            wrappedSceneTimeF(double elapsingTime) {
    return static_cast<float>(std::fmod(elapsingTime, kSceneTimeWrapPeriodSec));
}

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

        // RT name resolution is hoisted from the per-frame UpdateUniforms
        // hot path to InitUniforms.  rt_valid carries "we found this slot's
        // RT in Scene::renderTargets at prepare time"; the width / height /
        // mipmap_level are the resolved tuple uploaded by Apply.
        //
        // Population:
        //   - link-tex names ("_rt_link_N") are pre-resolved to their
        //     "_rt_offscreen_N" target's dimensions so Apply has nothing to
        //     compute.
        //   - on an RT miss (name absent from Scene::renderTargets at
        //     prepare), rt_valid stays false and Apply skips the slot —
        //     same behaviour as the original `continue` branch.
        //
        // Invariant: Scene::renderTargets is parse-time-immutable
        // post-WPSceneParser.  The single legal post-parse write is
        // CopyPass::prepare inserting a copy of an existing RT and toggling
        // its allowReuse flag — width / height / mipmap_level are never
        // mutated.  Each WPShaderValueUpdater is constructed fresh per
        // scene (WPSceneParser), so the cache resets implicitly on scene
        // swap; no cross-scene staleness.
        bool rt_valid { false };
        i32  rt_width { 0 };
        i32  rt_height { 0 };
        uint rt_mipmap_level { 1 };
    };
    std::array<Tex, 12> texs;
};

// Per-(node,camera) cache of the model/MVP/VP uniform block.  Re-uploaded
// verbatim while the node transform + camera VP epochs are unchanged and
// neither parallax nor camera-shake is active, so the two double-precision
// 4x4 inverses (g_ModelMatrixInverse / g_ModelViewProjectionMatrixInverse)
// and the four fromMatrix conversions run once per CHANGE, not per frame.
struct WPNodeMatrixCache {
    uint64_t    node_epoch { 0 };
    uint64_t    vp_epoch { 0 };
    bool        valid { false };
    bool        has_vp { false }, has_m { false }, has_am { false };
    bool        has_mi { false }, has_mvp { false }, has_mvpi { false };
    ShaderValue vp, m, am, mi, mvp, mvpi;
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

// "this scene consumes audio" predicate.  The per-frame audio FFT
// (AudioAnalyzer::Process) + the emit-rate scan must run iff SOMETHING reads
// the spectrum: a shader uniform (has_AUDIOSPECTRUM*), an audio-reactive
// particle subsystem, OR a SceneScript audio buffer registration
// (engine._audioRegs non-empty).  CRITICAL: all three terms are required —
// omitting the script-audio term starves script-only-audio scenes (their
// _audioRegs consumer would get no data).
inline bool audioConsumerPredicate(bool hasUniform, bool hasReactiveParticle, bool hasScriptAudio) {
    return hasUniform || hasReactiveParticle || hasScriptAudio;
}

// True iff ANY subsystem in [begin,end) is audio-reactive.  Computed
// once at scene build and stored on Scene so the render thread reads it without
// re-scanning particleSubByNodeId.  Iterates a map<id, ParticleSubSystem*>.
template<class It>
inline bool anyAudioReactive(It begin, It end) {
    for (It it = begin; it != end; ++it)
        if (it->second && it->second->IsAudioReactive()) return true;
    return false;
}

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
    // Audio-consumer terms.  m_hasAudioUniform is OR'd from the
    // per-node has_AUDIOSPECTRUM* bits in InitUniforms; these two are pushed in
    // from the scene-build / SceneObject side.  Process() runs iff
    // hasAudioConsumer() — non-audio scenes skip the per-frame FFT entirely.
    void SetHasReactiveParticles(bool v) { m_hasReactiveParticles = v; }
    // Written from the QML thread (SceneObject::setupTextScripts, fired on
    // firstFrame) and read on the render thread (FrameBegin); atomic to avoid a
    // data race.  Relaxed: a one-frame delay before the producer observes it is
    // harmless (the first frame's audio buffers were already zero at startup).
    void SetHasScriptAudio(bool v) { m_hasScriptAudio.store(v, std::memory_order_relaxed); }
    bool hasAudioConsumer() const {
        return audioConsumerPredicate(m_hasAudioUniform,
                                      m_hasReactiveParticles,
                                      m_hasScriptAudio.load(std::memory_order_relaxed));
    }

    // Append events fired by a specific node's puppet animation layer during
    // this frame.  Called from the render thread; not thread-safe.
    void PushAnimationEvents(i32 nodeId, std::vector<WPPuppetLayer::PendingEvent> events);

    // Move all pending animation events out (and reset the queue).  Designed
    // to be called from the QML/script thread; holds a mutex to synchronise
    // with render-thread pushes.
    std::vector<PendingAnimationEvent> DrainAnimationEvents();

    void SetScreenSize(i32 w, i32 h) override { m_screen_size = { (float)w, (float)h }; }

    // Volumetric per-light uniform upload.  Iterates Scene::lights, calls op()
    // 5 times per volumetric light (slots 0..4 = g_RenderVar0..4).  v1 cuts at
    // Point/LPoint; LSpot/LTube/LDirectional are parsed-but-not-uploaded until
    // a future leg.  Safe to call zero or many times per frame - does not
    // mutate updater state; reads Scene::lights + each light's transform.
    // Calls UpdateTrans() on each volumetric light's node to walk the parent
    // chain.
    void UpdateVolumetricLightUniforms(const WritePerLightVarOp& op);

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
    // Audio-consumer aggregate (see audioConsumerPredicate).  The
    // updater is created fresh per scene (WPSceneParser), so these start false
    // and latch as consumers are detected; no reset needed.
    bool              m_hasAudioUniform { false };
    bool              m_hasReactiveParticles { false };
    std::atomic<bool> m_hasScriptAudio { false };

    // Per-node lookups are point queries (find/at/exists), never ordered
    // iteration — std::hash<void*> is typically a no-op cast so unordered_map
    // gives O(1) average lookup with zero extra cost vs the RB-tree probe
    // std::map issued for every uniform write per pass per frame.  Forward
    // hook: per-light state for a future lights/ambient split can live in a
    // sibling unordered_map keyed by SceneLight* without restructuring the
    // container contract.
    std::unordered_map<void*, WPShaderValueData> m_nodeDataMap;
    std::unordered_map<void*, WPUniformInfo>     m_nodeUniformInfoMap;
    // Keyed on (node ptr, cam_name): the SAME node renders under the global
    // camera (empty name), the "effect" camera, and "reflected_perspective"
    // in different passes within one frame, each producing a different
    // M/VP/MVP.  A node-only key would cross-contaminate those passes.
    Map<std::pair<void*, std::string>, WPNodeMatrixCache> m_nodeMatrixCache;

    std::mutex                         m_anim_events_mtx;
    std::vector<PendingAnimationEvent> m_anim_events;
};
} // namespace wallpaper
