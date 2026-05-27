#include "WPShaderValueUpdater.hpp"
#include "Eigen/src/Core/Matrix.h"
#include "Eigen/src/Geometry/Transform.h"
#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "Scene/SpriteSnapshotGate.h"
#include "SpriteAnimation.hpp"
#include "SpecTexs.hpp"
#include "Audio/AudioAnalyzer.h"
#include "Core/ArrayHelper.hpp"
#include "Utils/Algorism.h"
#include "Utils/SceneProfiler.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <iostream>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <numeric>
#include <set>

using namespace wallpaper;
using namespace Eigen;

void WPShaderValueUpdater::FrameBegin() {
    WEK_PROFILE_SCOPE("WPShaderValueUpdater::FrameBegin");
    /*
        using namespace std::chrono;
        auto nowTime = system_clock::to_time_t(system_clock::now());
        auto cTime   = std::localtime(&nowTime);
        m_dayTime =
            (((cTime->tm_hour * 60) + cTime->tm_min) * 60 + cTime->tm_sec) / (24.0f * 60.0f
       * 60.0f);
    */
    double new_time    = m_mouseDelayedTime + m_scene->frameTime;
    new_time           = new_time > m_parallax.delay ? m_parallax.delay : new_time;
    m_mouseDelayedTime = new_time;
    double t           = new_time / m_parallax.delay;
    m_mousePos         = std::array { (float)algorism::lerp(t, m_mousePos[0], m_mousePosInput[0]),
                              (float)algorism::lerp(t, m_mousePos[1], m_mousePosInput[1]) };

    // Compute camera shake offset (sum-of-sinusoids pseudo-noise)
    if (m_shake.enable) {
        float t = (float)m_scene->elapsingTime * m_shake.speed;
        float r = m_shake.roughness;
        float sx =
            std::sin(t * 1.0f) + std::sin(t * 2.3f + 1.7f) * r + std::sin(t * 4.7f + 3.1f) * r * r;
        float sy = std::cos(t * 1.3f + 0.5f) + std::cos(t * 2.7f + 2.1f) * r +
                   std::cos(t * 5.1f + 0.9f) * r * r;
        float norm    = 1.0f + r + r * r;
        m_shakeOffset = Vector2f(sx, sy) * (m_shake.amplitude / norm);
    }

    // Process audio FFT for this frame — only if the scene actually consumes
    // audio (a spectrum uniform, a reactive particle, or a SceneScript audio
    // buffer).  Non-audio scenes skip the full 512-sample x2 FFT every frame
    //.
    if (m_audioAnalyzer && hasAudioConsumer()) {
        m_audioAnalyzer->Process();
    }

    // Advance camera path animation
    if (m_scene->activeCamera && m_scene->activeCamera->HasPaths()) {
        m_scene->activeCamera->AdvanceTime(m_scene->frameTime);
    }
    // Keep reflected camera in sync
    if (auto it = m_scene->cameras.find("reflected_perspective"); it != m_scene->cameras.end()) {
        if (it->second->HasPaths()) {
            it->second->AdvanceTime(m_scene->frameTime);
        }
    }
}

void WPShaderValueUpdater::FrameEnd() {}

void WPShaderValueUpdater::MouseInput(double x, double y) {
    using namespace std::chrono;

    auto   now_time = steady_clock::now();
    double new_time = m_mouseDelayedTime -
                      duration_cast<duration<double>>(now_time - m_last_mouse_input_time).count();
    m_mouseDelayedTime = new_time < 0.0f ? 0.0f : new_time;

    m_mousePosInput[0] = (float)x;
    m_mousePosInput[1] = (float)y;

    m_last_mouse_input_time = now_time;
}

void WPShaderValueUpdater::InitUniforms(SceneNode* pNode, const ExistsUniformOp& existsOp) {
    m_nodeUniformInfoMap[pNode] = WPUniformInfo();
    auto& info                  = m_nodeUniformInfoMap[pNode];
    info.has_MI                 = existsOp(G_MI);
    info.has_M                  = existsOp(G_M);
    info.has_AM                 = existsOp(G_AM);
    info.has_MVP                = existsOp(G_MVP);
    info.has_MVPI               = existsOp(G_MVPI);
    info.has_ETVP               = existsOp(G_ETVP);
    info.has_ETVPI              = existsOp(G_ETVPI);

    info.has_VP = existsOp(G_VP);

    info.has_BONES              = existsOp(G_BONES);
    info.has_TIME               = existsOp(G_TIME);
    info.has_DAYTIME            = existsOp(G_DAYTIME);
    info.has_POINTERPOSITION    = existsOp(G_POINTERPOSITION);
    info.has_PARALLAXPOSITION   = existsOp(G_PARALLAXPOSITION);
    info.has_TEXELSIZE          = existsOp(G_TEXELSIZE);
    info.has_TEXELSIZEHALF      = existsOp(G_TEXELSIZEHALF);
    info.has_SCREEN             = existsOp(G_SCREEN);
    info.has_LIGHTAMBIENTCOLOR  = existsOp(G_LIGHTAMBIENTCOLOR);
    info.has_LIGHTSKYLIGHTCOLOR = existsOp(G_LIGHTSKYLIGHTCOLOR);
    info.has_LP                 = existsOp(G_LP);
    info.has_LCR                = existsOp(G_LCR);
    info.has_EYEPOSITION        = existsOp(G_EYEPOSITION);

    info.has_AUDIOSPECTRUM16LEFT  = existsOp(G_AUDIOSPECTRUM16LEFT);
    info.has_AUDIOSPECTRUM16RIGHT = existsOp(G_AUDIOSPECTRUM16RIGHT);
    info.has_AUDIOSPECTRUM32LEFT  = existsOp(G_AUDIOSPECTRUM32LEFT);
    info.has_AUDIOSPECTRUM32RIGHT = existsOp(G_AUDIOSPECTRUM32RIGHT);
    info.has_AUDIOSPECTRUM64LEFT  = existsOp(G_AUDIOSPECTRUM64LEFT);
    info.has_AUDIOSPECTRUM64RIGHT = existsOp(G_AUDIOSPECTRUM64RIGHT);

    if (info.has_AUDIOSPECTRUM16LEFT || info.has_AUDIOSPECTRUM16RIGHT ||
        info.has_AUDIOSPECTRUM32LEFT || info.has_AUDIOSPECTRUM32RIGHT ||
        info.has_AUDIOSPECTRUM64LEFT || info.has_AUDIOSPECTRUM64RIGHT) {
        m_hasAudioUniform = true; // scene-level aggregate FFT gate
        LOG_INFO("Audio spectrum uniforms detected for node %d", pNode->ID());
    }

    std::accumulate(begin(info.texs), end(info.texs), 0, [&existsOp](uint index, auto& value) {
        value.has_resolution = existsOp(WE_GLTEX_RESOLUTION_NAMES[index]);
        value.has_mipmap     = existsOp(WE_GLTEX_MIPMAPINFO_NAMES[index]);
        return index + 1;
    });
}

void WPShaderValueUpdater::UpdateUniforms(SceneNode* pNode, sprite_map_t& sprites,
                                          const UpdateUniformOp& updateOp) {
    UpdateUniforms(pNode, sprites, updateOp, "");
}

void WPShaderValueUpdater::UpdateUniforms(SceneNode* pNode, sprite_map_t& sprites,
                                          const UpdateUniformOp& updateOp,
                                          const std::string&     camera_override) {
    WEK_PROFILE_SCOPE("WPShaderValueUpdater::UpdateUniforms");
    if (! pNode->Mesh()) return;

    pNode->UpdateTrans();

    const SceneCamera* camera;
    std::string_view   cam_name;
    if (! camera_override.empty()) {
        cam_name = camera_override;
        camera   = m_scene->cameras.at(camera_override).get();
    } else {
        cam_name = pNode->Camera();
        if (! cam_name.empty()) {
            camera = m_scene->cameras.at(std::string(cam_name)).get();
        } else
            camera = m_scene->activeCamera;
    }

    if (! camera) return;

    auto* material = pNode->Mesh()->Material();
    if (! material) return;
    // auto& shadervs = material->customShader.updateValueList;
    // const auto& valueSet = material->customShader.valueSet;

    // Explicit guard replacing a data-driven assert that compiled out under
    // -DNDEBUG in every shipped build.  Without it, operator[] on an absent
    // node would silently default-construct a zeroed UniformInfo entry and
    // render with garbage uniforms (silent corruption).  Use .at() after the
    // existence check so no entry is ever inserted.
    if (! exists(m_nodeUniformInfoMap, pNode)) {
        LOG_ERROR("no uniform info for node %d, skipping uniform update", pNode->ID());
        return;
    }
    const auto& info = m_nodeUniformInfoMap.at(pNode);

    bool hasNodeData = exists(m_nodeDataMap, pNode);
    if (hasNodeData) {
        auto& nodeData = m_nodeDataMap.at(pNode);
        for (const auto& el : nodeData.renderTargets) {
            // Link textures (_rt_link_N) aren't directly in renderTargets;
            // resolve to the corresponding offscreen RT so we can look up
            // the correct dimensions for g_TextureNResolution.
            std::string rtName = el.second;
            if (IsSpecLinkTex(rtName)) {
                rtName = GenOffscreenRT(ParseLinkTex(rtName));
            }
            if (m_scene->renderTargets.count(rtName) == 0) {
                static std::set<std::string> _rt_miss_logged;
                if (_rt_miss_logged.insert(el.second).second) {
                    LOG_INFO("RT miss: tex[%zu]='%s' resolved='%s' NOT in renderTargets",
                             el.first,
                             el.second.c_str(),
                             rtName.c_str());
                }
                continue;
            }
            const auto& rt = m_scene->renderTargets[rtName];
            {
                static std::set<std::string> _rt_res_logged;
                if (IsSpecLinkTex(el.second) && _rt_res_logged.insert(el.second).second) {
                    LOG_INFO("RT resolution upload: tex[%zu]='%s' → '%s' (%dx%d)",
                             el.first,
                             el.second.c_str(),
                             rtName.c_str(),
                             rt.width,
                             rt.height);
                }
            }

            const auto& unifrom_tex = info.texs[el.first];

            if (unifrom_tex.has_resolution) {
                std::array<i32, 4> resolution_uint({ rt.width, rt.height, rt.width, rt.height });
                updateOp(WE_GLTEX_RESOLUTION_NAMES[el.first],
                         ShaderValue(array_cast<float>(resolution_uint)));
            }
            if (unifrom_tex.has_mipmap) {
                updateOp(WE_GLTEX_MIPMAPINFO_NAMES[el.first], (float)rt.mipmap_level);
            }
        }
        if (nodeData.puppet_layer.hasPuppet() && info.has_BONES) {
            auto        data          = nodeData.puppet_layer.genFrame(m_scene->frameTime);
            static bool _bones_logged = false;
            if (! _bones_logged) {
                LOG_INFO("uploading g_Bones: %zu bones, frameTime=%.4f",
                         data.size(),
                         m_scene->frameTime);
                _bones_logged = true;
            }
            // A puppet whose skeleton parsed to ZERO bones makes genFrame()
            // return an empty span; uploading it would index `data[0]` on an
            // empty std::span and abort the whole process (plasmashell crash
            // loop on totoro 2891663007, "uploading g_Bones: 0 bones").
            // boneAffinesAsUploadFloats() returns an empty span for the no-bone
            // case — upload only when non-empty (see WPPuppet.hpp).
            if (auto boneFloats = boneAffinesAsUploadFloats(data); ! boneFloats.empty()) {
                updateOp(G_BONES, boneFloats);
            }
            // Collect keyframe events fired during updateInterpolation inside
            // genFrame(); forwarded to the owning node's SceneScript via the
            // QML-side drain on the next evaluation tick.
            auto events = nodeData.puppet_layer.drainEvents();
            if (! events.empty()) {
                PushAnimationEvents(pNode->ID(), std::move(events));
            }
        } else {
            static bool _no_bones_logged = false;
            if (! _no_bones_logged && (nodeData.puppet_layer.hasPuppet() || info.has_BONES)) {
                LOG_INFO("BONES NOT uploaded: hasPuppet=%d has_BONES=%d",
                         (int)nodeData.puppet_layer.hasPuppet(),
                         (int)info.has_BONES);
                _no_bones_logged = true;
            }
        }
    }

    bool reqMI    = info.has_MI;
    bool reqM     = info.has_M;
    bool reqAM    = info.has_AM;
    bool reqMVP   = info.has_MVP;
    bool reqMVPI  = info.has_MVPI;
    bool reqETVP  = info.has_ETVP;
    bool reqETVPI = info.has_ETVPI;

    // ---- matrix/VP uniform block: recompute only when something moved ----
    // Parallax mutates the model matrix from live mouse input every frame;
    // camera-shake mutates the VP for the global camera every frame.  Both
    // must stay volatile (never served from the static cache).  The flags
    // mirror the exact runtime conditions of the parallax/shake math below.
    const bool parallaxActive = m_parallax.enable && hasNodeData && cam_name != "effect";
    const bool shakeActive    = m_shake.enable && cam_name.empty();

    auto&      mc        = m_nodeMatrixCache[{ pNode, std::string(cam_name) }];
    const bool recompute = uniformMatricesShouldRecompute(! mc.valid,
                                                          parallaxActive,
                                                          shakeActive,
                                                          pNode->TransEpoch(),
                                                          mc.node_epoch,
                                                          camera->VpEpoch(),
                                                          mc.vp_epoch);

    // Recompute-vs-cached counter (gated on WEKDE_DEBUG_UNIFORM): confirms a
    // static scene logs ~100% cached after the first frame and a parallax
    // scene logs ~100% recompute, without instrumenting the production path.
    {
        static const bool _unf_dbg = std::getenv("WEKDE_DEBUG_UNIFORM") != nullptr;
        if (_unf_dbg) {
            static uint64_t _unf_recompute = 0, _unf_cached = 0;
            if (recompute)
                ++_unf_recompute;
            else
                ++_unf_cached;
            if ((_unf_recompute + _unf_cached) % 90 == 0) {
                LOG_INFO("uniform matrix gate: recompute=%llu cached=%llu",
                         (unsigned long long)_unf_recompute,
                         (unsigned long long)_unf_cached);
            }
        }
    }

    if (! recompute) {
        // Static fast path: re-upload the cached matrices, skipping the
        // recompute AND both double 4x4 inverses.
        if (info.has_VP && mc.has_vp) updateOp(G_VP, mc.vp);
        if (reqM && mc.has_m) updateOp(G_M, mc.m);
        if (reqAM && mc.has_am) updateOp(G_AM, mc.am);
        if (reqMI && mc.has_mi) updateOp(G_MI, mc.mi);
        if (reqMVP && mc.has_mvp) updateOp(G_MVP, mc.mvp);
        if (reqMVPI && mc.has_mvpi) updateOp(G_MVPI, mc.mvpi);
    } else {
        WEK_PROFILE_SCOPE("UpdateUniforms::matrixRecompute");
        Matrix4d viewProTrans = camera->GetViewProjectionMatrix();

        // Camera shake: translate the view-projection so all scene objects shift together.
        // Only apply to the global camera (cam_name empty) — per-node effect cameras and
        // the "effect" camera render to intermediate RTs and must not be shaken.
        if (m_shake.enable && cam_name.empty()) {
            Vector2f shakeVec;
            if (camera->IsPerspective()) {
                // Perspective: m_shakeOffset already contains amplitude, use directly
                // as world-space translation (typical amplitude 0.01 ≈ subtle shift)
                shakeVec = m_shakeOffset;
            } else {
                // Ortho: scale by ortho dimensions for pixel-space shake
                Vector2f ortho { (float)m_scene->ortho[0], (float)m_scene->ortho[1] };
                shakeVec = m_shakeOffset.cwiseProduct(ortho) * 0.01f;
            }
            viewProTrans =
                viewProTrans *
                Affine3d(Translation3d(Vector3d(shakeVec.x(), shakeVec.y(), 0.0f))).matrix();
        }

        if (info.has_VP) {
            mc.vp     = ShaderValue::fromMatrix(viewProTrans);
            mc.has_vp = true;
            updateOp(G_VP, mc.vp);
        }
        if (reqM || reqMVP || reqMI || reqMVPI) {
            Matrix4d modelTrans = pNode->ModelTrans();
            if (hasNodeData && cam_name != "effect") {
                const auto& nodeData = m_nodeDataMap.at(pNode);
                if (m_parallax.enable) {
                    Vector3f nodePos = pNode->Translate();
                    Vector2f depth(&nodeData.parallaxDepth[0]);
                    Vector2f ortho { (float)m_scene->ortho[0], (float)m_scene->ortho[1] };
                    // flip mouse y axis
                    Vector2f mouseVec =
                        Scaling(1.0f, -1.0f) * (Vector2f { 0.5f, 0.5f } - Vector2f(&m_mousePos[0]));
                    mouseVec        = mouseVec.cwiseProduct(ortho) * m_parallax.mouseinfluence;
                    Vector3f camPos = camera->GetPosition().cast<float>();
                    // WE parallax: position-based + mouse-based shift, both
                    // scaled by per-layer parallaxDepth and scene amount.  The
                    // position-based term (nodePos - camPos) is intentional —
                    // WE's editor preview shows layers already at their shifted
                    // position (so authors place layers KNOWING about the
                    // parallax bake-in).  Removing it causes layers authored
                    // for the shifted position (e.g. 2866203962 'Cyberpunk' R
                    // below Lucy's chin) to render above their intended spot.
                    Vector2f paraVec =
                        (nodePos.head<2>() - camPos.head<2>() + mouseVec).cwiseProduct(depth) *
                        m_parallax.amount;
                    modelTrans =
                        Affine3d(Translation3d(Vector3d(paraVec.x(), paraVec.y(), 0.0f))).matrix() *
                        modelTrans;
                }
            }

            if (reqM) {
                mc.m     = ShaderValue::fromMatrix(modelTrans);
                mc.has_m = true;
                updateOp(G_M, mc.m);
            }
            if (reqAM) {
                mc.am     = ShaderValue::fromMatrix(modelTrans);
                mc.has_am = true;
                updateOp(G_AM, mc.am);
            }
            if (reqMI) {
                mc.mi     = ShaderValue::fromMatrix(modelTrans.inverse());
                mc.has_mi = true;
                updateOp(G_MI, mc.mi);
            }

            // Diagnostic for nodes using separate M + VP (3D models with custom shaders)
            if (reqM && info.has_VP && ! reqMVP && cam_name.empty()) {
                static std::set<SceneNode*> _mvp_sep_logged;
                if (_mvp_sep_logged.insert(pNode).second) {
                    Vector4d center = viewProTrans * modelTrans * Vector4d(0, 0, 0, 1);
                    auto     t      = pNode->Translate();
                    auto     eyePos = camera->GetPosition();
                    LOG_INFO("M+VP diag id=%d translate=(%.1f,%.1f,%.1f) "
                             "clip_center=(%.3f,%.3f,%.3f,%.3f) "
                             "ndc=(%.3f,%.3f) eye=(%.3f,%.3f,%.3f) "
                             "depth=%d cull=%s",
                             pNode->ID(),
                             t.x(),
                             t.y(),
                             t.z(),
                             center.x(),
                             center.y(),
                             center.z(),
                             center.w(),
                             center.x() / center.w(),
                             center.y() / center.w(),
                             eyePos.x(),
                             eyePos.y(),
                             eyePos.z(),
                             (int)material->depthTest,
                             material->cullmode.c_str());
                }
            }
            if (reqMVP) {
                Matrix4d mvpTrans = viewProTrans * modelTrans;
                mc.mvp            = ShaderValue::fromMatrix(mvpTrans);
                mc.has_mvp        = true;
                updateOp(G_MVP, mc.mvp);
                if (reqMVPI) {
                    mc.mvpi     = ShaderValue::fromMatrix(mvpTrans.inverse());
                    mc.has_mvpi = true;
                    updateOp(G_MVPI, mc.mvpi);
                }
                // One-time diagnostic: log MVP info for nodes with empty camera (final composites)
                static std::set<SceneNode*> _mvp_logged;
                if (cam_name.empty() && _mvp_logged.insert(pNode).second) {
                    Vector4d center = mvpTrans * Vector4d(0, 0, 0, 1);
                    auto     t      = pNode->Translate();
                    LOG_INFO(
                        "MVP diag id=%d cam='' translate=(%.1f,%.1f) model=[%.3f,%.3f,%.3f,%.3f] "
                        "clip_center=(%.3f,%.3f) blend=%d",
                        pNode->ID(),
                        t.x(),
                        t.y(),
                        modelTrans(0, 0),
                        modelTrans(1, 1),
                        modelTrans(0, 3),
                        modelTrans(1, 3),
                        center.x() / center.w(),
                        center.y() / center.w(),
                        (int)material->blenmode);
                }
            }
            if (reqETVP || reqETVPI) {
                /*
                Vector3d nodePos = pNode->Translate().cast<double>();
                nodePos.z()      = 1.0f;
                Matrix4d etvpTrans =
                    viewProTrans * modelTrans * Affine3d(Eigen::Scaling(nodePos)).matrix();
                if (reqETVPI) updateOp(G_ETVP, ShaderValue::fromMatrix(etvpTrans));
                if (reqETVPI) updateOp(G_ETVPI, ShaderValue::fromMatrix(etvpTrans.inverse()));
                */
            }
        }
        mc.node_epoch = pNode->TransEpoch();
        mc.vp_epoch   = camera->VpEpoch();
        mc.valid      = true;
    }
    // ---- end matrix/VP block; per-frame-volatile uniforms continue below ----

    //	g_EffectTextureProjectionMatrix
    // shadervs.push_back({"g_EffectTextureProjectionMatrixInverse",
    // ShaderValue::ValueOf(Eigen::Matrix4f::Identity())});
    if (info.has_TIME) updateOp(G_TIME, (float)m_scene->elapsingTime);

    if (info.has_DAYTIME) updateOp(G_DAYTIME, (float)m_dayTime);

    if (info.has_POINTERPOSITION) updateOp(G_POINTERPOSITION, m_mousePos);

    if (info.has_TEXELSIZE) updateOp(G_TEXELSIZE, m_texelSize);

    if (info.has_TEXELSIZEHALF)
        updateOp(G_TEXELSIZEHALF, std::array { m_texelSize[0] / 2.0f, m_texelSize[1] / 2.0f });

    if (info.has_SCREEN)
        updateOp(G_SCREEN,
                 std::array<float, 3> {
                     m_screen_size[0], m_screen_size[1], m_screen_size[0] / m_screen_size[1] });

    if (info.has_PARALLAXPOSITION) {
        // g_ParallaxPosition is the mouse-driven [0,1] input that effect
        // shaders (e.g. depthparallax) consume as "where is the viewer
        // looking".  Mouse at screen center (0.5, 0.5) must produce (0.5,
        // 0.5) so depth-based pixel displacement is zero — otherwise the
        // depth map shifts foreground pixels relative to background even
        // at rest, creating gaps between adjacent layers of differing
        // depth (3363252053 dress (foreground) vs water/ground (background)
        // showed a ~7-pixel horizontal seam because the prior formula
        // `Scaling(1,-1) * mouseY - 0.5` evaluated to -1 at mouseY=0.5,
        // giving g_ParallaxPosition.y = 0.5 + (-1) * 0.30 = 0.20).
        //
        // Correct Y-flip is `1 - mouseY` (mouse Y is 0-at-top while world
        // Y is up), then offset from center scaled by mouseinfluence.
        Vector2f mouseCentered { m_mousePos[0] - 0.5f, 0.5f - m_mousePos[1] };
        Vector2f para =
            Vector2f { 0.5f, 0.5f } + mouseCentered * m_parallax.mouseinfluence;
        updateOp(G_PARALLAXPOSITION, std::array { para[0], para[1] });
    }

    // Apply SceneScript-driven sprite frame pinning before the per-frame
    // GetAnimateFrame call.  Each pass holds its own copy of the sprite
    // animation (sprites_map is by value), so the pin must be applied
    // every frame for every pass that renders this node.  See
    // Scene::nodeSpriteFrame for the source-of-truth map populated by
    // SceneScript writes through SceneWallpaper::setLayerSpriteFrame.
    if (pNode) {
        auto sfit = m_scene->nodeSpriteFrame.find(pNode->ID());
        if (sfit != m_scene->nodeSpriteFrame.end()) {
            const auto& [wantsManual, frameIdx] = sfit->second;
            for (auto& [i, sp] : sprites) {
                if (wantsManual)
                    sp.SetManualFrame(frameIdx);
                else if (sp.isManualFrame())
                    sp.ClearManualFrame();
            }
        } else {
            // No override entry — make sure any prior manual pin is released.
            for (auto& [i, sp] : sprites) {
                if (sp.isManualFrame()) sp.ClearManualFrame();
            }
        }
    }
    for (auto& [i, sp] : sprites) {
        const auto& f      = sp.GetAnimateFrame(m_scene->frameTime);
        auto        grot   = WE_GLTEX_ROTATION_NAMES[i];
        auto        gtrans = WE_GLTEX_TRANSLATION_NAMES[i];
        updateOp(grot, std::array { f.xAxis[0], f.xAxis[1], f.yAxis[0], f.yAxis[1] });
        updateOp(gtrans, std::array { f.x, f.y });
    }

    // Publish a per-node sprite snapshot for the SceneScript bridge
    // (thisLayer.getTextureAnimation().frameCount / duration / getFrame() /
    // isPlaying()).  Uses the first sprite as the representative — multiple
    // sprites on the same layer share authored frametimes and advance in
    // lock-step within a tick, so the first is faithful for read-back.
    // Consumer-gate: skip the publication (mutex + map probe + 4 writes) when
    // no SceneScript has called thisLayer.getTextureAnimation() yet.  Sticky-on
    // once a consumer appears; first-frame stale-zero on the consumer side is
    // harmless (default-constructed NodeSpriteSnapshot matches the pre-existing
    // cache-miss return path).
    if (pNode && ! sprites.empty() && spriteSnapshotShouldPublish(m_scene->needsSpriteSnapshot)) {
        const auto&                   first_sp = sprites.begin()->second;
        std::lock_guard<std::mutex>   lock(m_scene->nodeSpriteSnapshotMutex);
        auto&                         snap     = m_scene->nodeSpriteSnapshot[pNode->ID()];
        snap.numFrames                         = (u32)first_sp.numFrames();
        snap.currentFrame                      = (u32)first_sp.curFrameIndex();
        snap.duration                          = first_sp.totalDuration();
        snap.isManualPin                       = first_sp.isManualFrame();
    }

    if (info.has_LP || info.has_LCR) {
        std::array<float, 16> lights { 0 };
        std::array<float, 16> lights_color_radius { 0 };
        std::array<float, 12> lights_color { 0 };
        uint                  i              = 0;
        bool                  reflect_lights = (cam_name == "reflected_perspective");
        for (auto& l : m_scene->lights) {
            if (i == 4) break;
            assert(l->node() != nullptr);
            // Walk the parent chain so parented lights upload their WORLD
            // translation, not the always-zero local Translate().  Real-Time
            // Earth (3557068717) drives this: 2 point lights parented to the
            // animated SUN m5 node — without world-position upload they read
            // back as (0,0,0) and the planet's outward normals dot to zero,
            // rendering the surface pure black.  UpdateTrans() is a no-op
            // when not dirty, so the per-frame cost on flat scenes is one
            // cache hit.
            l->node()->UpdateTrans();
            const auto& world = l->node()->ModelTrans();
            lights[i * 4 + 0] = (float)world(0, 3);
            lights[i * 4 + 1] = (float)world(1, 3);
            lights[i * 4 + 2] = (float)world(2, 3);
            // Pack per-light falloff exponent into .w — the modern WE pipeline
            // convention.  PerformLighting_V1 reads it as the `exponent`
            // argument to ComputePBRLightShadow's `pow(saturate(1-d/r), e)`
            // falloff.  SceneLight defaults exponent to 1.0 (linear) for
            // legacy scenes; Real-Time Earth authors 0.1 for soft long-tail.
            lights[i * 4 + 3] = l->exponent();
            // Reflect light Y about the floor plane so the underside of
            // objects receives the dominant lighting in the reflection.
            if (reflect_lights) lights[i * 4 + 1] = -lights[i * 4 + 1];
            // g_LightsColorRadius: vec4(color * intensity, radius)
            const auto& ci                 = l->colorIntensity();
            lights_color_radius[i * 4 + 0] = ci[0];
            lights_color_radius[i * 4 + 1] = ci[1];
            lights_color_radius[i * 4 + 2] = ci[2];
            lights_color_radius[i * 4 + 3] = l->radius();
            if (i < 3) {
                const auto& color = l->premultipliedColor();
                std::copy(color.begin(), color.end(), lights_color.begin() + i * 4);
            }
            i++;
        }
        updateOp(G_LP, lights);
        updateOp(G_LCR, lights_color_radius);
        updateOp(G_LCP, lights_color);
    }

    if (info.has_LIGHTAMBIENTCOLOR) updateOp(G_LIGHTAMBIENTCOLOR, m_scene->ambientColor);
    if (info.has_LIGHTSKYLIGHTCOLOR) updateOp(G_LIGHTSKYLIGHTCOLOR, m_scene->skylightColor);

    if (info.has_EYEPOSITION && camera->IsPerspective()) {
        auto pos = camera->GetPosition();
        updateOp(G_EYEPOSITION, std::array { (float)pos.x(), (float)pos.y(), (float)pos.z() });
    }

    // Audio spectrum uniforms — write zeros when no data available to avoid
    // uninitialized UBO reads that produce noise in audio visualizer effects.
    if (m_audioAnalyzer && m_audioAnalyzer->HasData()) {
        if (info.has_AUDIOSPECTRUM16LEFT)
            updateOp(G_AUDIOSPECTRUM16LEFT, m_audioAnalyzer->GetSpectrum16Left());
        if (info.has_AUDIOSPECTRUM16RIGHT)
            updateOp(G_AUDIOSPECTRUM16RIGHT, m_audioAnalyzer->GetSpectrum16Right());
        if (info.has_AUDIOSPECTRUM32LEFT)
            updateOp(G_AUDIOSPECTRUM32LEFT, m_audioAnalyzer->GetSpectrum32Left());
        if (info.has_AUDIOSPECTRUM32RIGHT)
            updateOp(G_AUDIOSPECTRUM32RIGHT, m_audioAnalyzer->GetSpectrum32Right());
        if (info.has_AUDIOSPECTRUM64LEFT)
            updateOp(G_AUDIOSPECTRUM64LEFT, m_audioAnalyzer->GetSpectrum64Left());
        if (info.has_AUDIOSPECTRUM64RIGHT)
            updateOp(G_AUDIOSPECTRUM64RIGHT, m_audioAnalyzer->GetSpectrum64Right());
    } else {
        // Zero-fill when audio analyzer isn't ready yet
        static const std::array<float, 64> zeros {};
        if (info.has_AUDIOSPECTRUM16LEFT) updateOp(G_AUDIOSPECTRUM16LEFT, zeros);
        if (info.has_AUDIOSPECTRUM16RIGHT) updateOp(G_AUDIOSPECTRUM16RIGHT, zeros);
        if (info.has_AUDIOSPECTRUM32LEFT) updateOp(G_AUDIOSPECTRUM32LEFT, zeros);
        if (info.has_AUDIOSPECTRUM32RIGHT) updateOp(G_AUDIOSPECTRUM32RIGHT, zeros);
        if (info.has_AUDIOSPECTRUM64LEFT) updateOp(G_AUDIOSPECTRUM64LEFT, zeros);
        if (info.has_AUDIOSPECTRUM64RIGHT) updateOp(G_AUDIOSPECTRUM64RIGHT, zeros);
    }
}

void WPShaderValueUpdater::SetNodeData(void* nodeAddr, const WPShaderValueData& data) {
    m_nodeDataMap[nodeAddr] = data;
}

void WPShaderValueUpdater::PushAnimationEvents(i32                                      nodeId,
                                               std::vector<WPPuppetLayer::PendingEvent> events) {
    if (events.empty()) return;
    std::lock_guard<std::mutex> lock(m_anim_events_mtx);
    m_anim_events.reserve(m_anim_events.size() + events.size());
    for (auto& e : events) {
        m_anim_events.push_back({ nodeId, e.frame, std::move(e.name) });
    }
}

std::vector<PendingAnimationEvent> WPShaderValueUpdater::DrainAnimationEvents() {
    std::vector<PendingAnimationEvent> out;
    std::lock_guard<std::mutex>        lock(m_anim_events_mtx);
    out.swap(m_anim_events);
    return out;
}

void WPShaderValueUpdater::SetTexelSize(float x, float y) { m_texelSize = { x, y }; }

void WPShaderValueUpdater::UpdateVolumetricLightUniforms(const WritePerLightVarOp& op) {
    WEK_PROFILE_SCOPE("WPShaderValueUpdater::UpdateVolumetricLightUniforms");
    if (! m_scene) return;
    // candidate_idx aligns with WPSceneParser-populated
    // volumetricsConfig.per_light: both arrays are built by walking
    // scene.lights with the same isVolumetricEmitterCandidate() predicate in
    // the same order, so the Nth emitter here is per_light[N] over there.
    int candidate_idx = 0;
    for (auto& l : m_scene->lights) {
        if (! l) continue;
        // Single predicate (cast-flag + kind ∈ {Point, LPoint}) so this
        // writer agrees with parser-side per_light gating + the public
        // Scene::volumetricLights() accessor.
        if (! l->isVolumetricEmitterCandidate()) continue;
        if (! l->node()) continue;
        l->node()->UpdateTrans();
        const auto& world = l->node()->ModelTrans();

        // g_RenderVar0: shadowmap transforms - no shadow path in v1.
        std::array<float, 4> r0 { 0.0f, 0.0f, 0.0f, 0.0f };
        // g_RenderVar1: (radius, spot_inner, spot_outer, intensity).
        // v1 only emits Point/LPoint; spot inner/outer stay at 0.
        std::array<float, 4> r1 { l->radius(), 0.0f, 0.0f, l->intensity() };
        // g_RenderVar2: (world_origin.xyz, density).
        std::array<float, 4> r2 { (float)world(0, 3), (float)world(1, 3),
                                  (float)world(2, 3), l->volumetric().density };
        // g_RenderVar3: (forward.xyz, point_proj_info).
        // Point/LPoint have no orientation; pack (0, 0, 1, 0) for shader-side
        // determinism.
        std::array<float, 4> r3 { 0.0f, 0.0f, 1.0f, 0.0f };
        // g_RenderVar4: (color * intensity, volumetricsexponent).
        const auto& ci = l->colorIntensity();
        std::array<float, 4> r4 { ci[0], ci[1], ci[2], l->volumetric().exponent };

        op(l.get(), candidate_idx, 0, r0);
        op(l.get(), candidate_idx, 1, r1);
        op(l.get(), candidate_idx, 2, r2);
        op(l.get(), candidate_idx, 3, r3);
        op(l.get(), candidate_idx, 4, r4);
        ++candidate_idx;
    }
}
