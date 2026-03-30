#include "WPShaderValueUpdater.hpp"
#include "Eigen/src/Core/Matrix.h"
#include "Eigen/src/Geometry/Transform.h"
#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "SpriteAnimation.hpp"
#include "SpecTexs.hpp"
#include "Core/ArrayHelper.hpp"
#include "Utils/Algorism.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <iostream>
#include <chrono>
#include <ctime>
#include <numeric>
#include <set>

using namespace wallpaper;
using namespace Eigen;

void WPShaderValueUpdater::FrameBegin() {
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
        float t   = (float)m_scene->elapsingTime * m_shake.speed;
        float r   = m_shake.roughness;
        float sx  = std::sin(t * 1.0f) + std::sin(t * 2.3f + 1.7f) * r +
                    std::sin(t * 4.7f + 3.1f) * r * r;
        float sy  = std::cos(t * 1.3f + 0.5f) + std::cos(t * 2.7f + 2.1f) * r +
                    std::cos(t * 5.1f + 0.9f) * r * r;
        float norm = 1.0f + r + r * r;
        m_shakeOffset = Vector2f(sx, sy) * (m_shake.amplitude / norm);
    }

    // Process audio FFT for this frame
    if (m_audioAnalyzer) {
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

    info.has_BONES            = existsOp(G_BONES);
    info.has_TIME             = existsOp(G_TIME);
    info.has_DAYTIME          = existsOp(G_DAYTIME);
    info.has_POINTERPOSITION  = existsOp(G_POINTERPOSITION);
    info.has_PARALLAXPOSITION = existsOp(G_PARALLAXPOSITION);
    info.has_TEXELSIZE        = existsOp(G_TEXELSIZE);
    info.has_TEXELSIZEHALF    = existsOp(G_TEXELSIZEHALF);
    info.has_SCREEN           = existsOp(G_SCREEN);
    info.has_LIGHTAMBIENTCOLOR  = existsOp(G_LIGHTAMBIENTCOLOR);
    info.has_LIGHTSKYLIGHTCOLOR = existsOp(G_LIGHTSKYLIGHTCOLOR);
    info.has_LP               = existsOp(G_LP);
    info.has_LCR              = existsOp(G_LCR);
    info.has_EYEPOSITION      = existsOp(G_EYEPOSITION);

    info.has_AUDIOSPECTRUM16LEFT  = existsOp(G_AUDIOSPECTRUM16LEFT);
    info.has_AUDIOSPECTRUM16RIGHT = existsOp(G_AUDIOSPECTRUM16RIGHT);
    info.has_AUDIOSPECTRUM32LEFT  = existsOp(G_AUDIOSPECTRUM32LEFT);
    info.has_AUDIOSPECTRUM32RIGHT = existsOp(G_AUDIOSPECTRUM32RIGHT);
    info.has_AUDIOSPECTRUM64LEFT  = existsOp(G_AUDIOSPECTRUM64LEFT);
    info.has_AUDIOSPECTRUM64RIGHT = existsOp(G_AUDIOSPECTRUM64RIGHT);

    if (info.has_AUDIOSPECTRUM16LEFT || info.has_AUDIOSPECTRUM16RIGHT ||
        info.has_AUDIOSPECTRUM32LEFT || info.has_AUDIOSPECTRUM32RIGHT ||
        info.has_AUDIOSPECTRUM64LEFT || info.has_AUDIOSPECTRUM64RIGHT) {
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
                                          const std::string& camera_override) {
    if (! pNode->Mesh()) return;

    pNode->UpdateTrans();

    const SceneCamera*  camera;
    std::string_view    cam_name;
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

    assert(exists(m_nodeUniformInfoMap, pNode));
    const auto& info = m_nodeUniformInfoMap[pNode];

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
                             el.first, el.second.c_str(), rtName.c_str());
                }
                continue;
            }
            const auto& rt = m_scene->renderTargets[rtName];
            {
                static std::set<std::string> _rt_res_logged;
                if (IsSpecLinkTex(el.second) && _rt_res_logged.insert(el.second).second) {
                    LOG_INFO("RT resolution upload: tex[%zu]='%s' → '%s' (%dx%d)",
                             el.first, el.second.c_str(), rtName.c_str(), rt.width, rt.height);
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
            auto data = nodeData.puppet_layer.genFrame(m_scene->frameTime);
            static bool _bones_logged = false;
            if (!_bones_logged) {
                LOG_INFO("uploading g_Bones: %zu bones, frameTime=%.4f", data.size(), m_scene->frameTime);
                _bones_logged = true;
            }
            updateOp(G_BONES, std::span<const float> { data[0].data(), data.size() * 16 });
        } else {
            static bool _no_bones_logged = false;
            if (!_no_bones_logged && (nodeData.puppet_layer.hasPuppet() || info.has_BONES)) {
                LOG_INFO("BONES NOT uploaded: hasPuppet=%d has_BONES=%d",
                         (int)nodeData.puppet_layer.hasPuppet(), (int)info.has_BONES);
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
        viewProTrans = viewProTrans *
            Affine3d(Translation3d(Vector3d(shakeVec.x(), shakeVec.y(), 0.0f))).matrix();
    }

    if (info.has_VP) {
        updateOp(G_VP, ShaderValue::fromMatrix(viewProTrans));
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
                Vector2f paraVec =
                    (nodePos.head<2>() - camPos.head<2>() + mouseVec).cwiseProduct(depth) *
                    m_parallax.amount;
                modelTrans =
                    Affine3d(Translation3d(Vector3d(paraVec.x(), paraVec.y(), 0.0f))).matrix() *
                    modelTrans;
            }
        }

        if (reqM) updateOp(G_M, ShaderValue::fromMatrix(modelTrans));
        if (reqAM) updateOp(G_AM, ShaderValue::fromMatrix(modelTrans));
        if (reqMI) updateOp(G_MI, ShaderValue::fromMatrix(modelTrans.inverse()));

        // Diagnostic for nodes using separate M + VP (3D models with custom shaders)
        if (reqM && info.has_VP && !reqMVP && cam_name.empty()) {
            static std::set<SceneNode*> _mvp_sep_logged;
            if (_mvp_sep_logged.insert(pNode).second) {
                Vector4d center = viewProTrans * modelTrans * Vector4d(0, 0, 0, 1);
                auto     t      = pNode->Translate();
                auto     eyePos = camera->GetPosition();
                LOG_INFO("M+VP diag id=%d translate=(%.1f,%.1f,%.1f) "
                         "clip_center=(%.3f,%.3f,%.3f,%.3f) "
                         "ndc=(%.3f,%.3f) eye=(%.3f,%.3f,%.3f) "
                         "depth=%d cull=%s",
                         pNode->ID(), t.x(), t.y(), t.z(),
                         center.x(), center.y(), center.z(), center.w(),
                         center.x() / center.w(), center.y() / center.w(),
                         eyePos.x(), eyePos.y(), eyePos.z(),
                         (int)material->depthTest, material->cullmode.c_str());
            }
        }
        if (reqMVP) {
            Matrix4d mvpTrans = viewProTrans * modelTrans;
            updateOp(G_MVP, ShaderValue::fromMatrix(mvpTrans));
            if (reqMVPI) updateOp(G_MVPI, ShaderValue::fromMatrix(mvpTrans.inverse()));
            // One-time diagnostic: log MVP info for nodes with empty camera (final composites)
            static std::set<SceneNode*> _mvp_logged;
            if (cam_name.empty() && _mvp_logged.insert(pNode).second) {
                Vector4d center = mvpTrans * Vector4d(0, 0, 0, 1);
                auto     t      = pNode->Translate();
                LOG_INFO("MVP diag id=%d cam='' translate=(%.1f,%.1f) model=[%.3f,%.3f,%.3f,%.3f] "
                         "clip_center=(%.3f,%.3f) blend=%d",
                         pNode->ID(), t.x(), t.y(),
                         modelTrans(0, 0), modelTrans(1, 1), modelTrans(0, 3), modelTrans(1, 3),
                         center.x() / center.w(), center.y() / center.w(),
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
        Vector2f para =
            Vector2f { 0.5f, 0.5f } +
            (Scaling(1.0f, -1.0f) * (Vector2f(&m_mousePos[0])) - Vector2f { 0.5f, 0.5f }) *
                m_parallax.mouseinfluence;
        updateOp(G_PARALLAXPOSITION, std::array { para[0], para[1] });
    }

    for (auto& [i, sp] : sprites) {
        const auto& f      = sp.GetAnimateFrame(m_scene->frameTime);
        auto        grot   = WE_GLTEX_ROTATION_NAMES[i];
        auto        gtrans = WE_GLTEX_TRANSLATION_NAMES[i];
        updateOp(grot, std::array { f.xAxis[0], f.xAxis[1], f.yAxis[0], f.yAxis[1] });
        updateOp(gtrans, std::array { f.x, f.y });
    }

    if (info.has_LP || info.has_LCR) {
        std::array<float, 16> lights { 0 };
        std::array<float, 16> lights_color_radius { 0 };
        std::array<float, 12> lights_color { 0 };
        uint                  i = 0;
        bool reflect_lights = (cam_name == "reflected_perspective");
        for (auto& l : m_scene->lights) {
            if (i == 4) break;
            assert(l->node() != nullptr);
            const auto& trans = l->node()->Translate();
            std::copy(trans.begin(), trans.end(), lights.begin() + i * 4);
            // Reflect light Y about the floor plane so the underside of
            // objects receives the dominant lighting in the reflection.
            if (reflect_lights) lights[i * 4 + 1] = -lights[i * 4 + 1];
            // g_LightsColorRadius: vec4(color * intensity, radius)
            const auto& ci = l->colorIntensity();
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

    if (info.has_LIGHTAMBIENTCOLOR)
        updateOp(G_LIGHTAMBIENTCOLOR, m_scene->ambientColor);
    if (info.has_LIGHTSKYLIGHTCOLOR)
        updateOp(G_LIGHTSKYLIGHTCOLOR, m_scene->skylightColor);

    if (info.has_EYEPOSITION && camera->IsPerspective()) {
        auto pos = camera->GetPosition();
        updateOp(G_EYEPOSITION, std::array { (float)pos.x(), (float)pos.y(), (float)pos.z() });
    }

    // Audio spectrum uniforms
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
    }
}

void WPShaderValueUpdater::SetNodeData(void* nodeAddr, const WPShaderValueData& data) {
    m_nodeDataMap[nodeAddr] = data;
}

void WPShaderValueUpdater::SetTexelSize(float x, float y) { m_texelSize = { x, y }; }
