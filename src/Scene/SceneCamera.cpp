#include "SceneCamera.h"
#include "SceneNode.h"
#include "Utils/Logging.h"
#include <algorithm>
#include <iostream>
#include "Utils/Eigen.h"

using namespace wallpaper;
using namespace Eigen;

void SceneCamera::SetDirectLookAt(const Vector3d& eye, const Vector3d& center, const Vector3d& up) {
    m_direct_lookat = true;
    m_eye           = eye;
    m_center        = center;
    m_up_vec        = up;
    Update();
}

Vector3d SceneCamera::GetPosition() const {
    if (m_direct_lookat) {
        if (m_reflect_y0) return { m_eye.x(), -m_eye.y(), m_eye.z() };
        return m_eye;
    }
    if (m_node) {
        return Affine3d(m_node->GetLocalTrans()) * Vector3d::Zero();
    }
    return Vector3d::Zero();
}

Vector3d SceneCamera::GetDirection() const {
    if (m_direct_lookat) {
        return (m_center - m_eye).normalized();
    }
    if (m_node) {
        return (m_node->GetLocalTrans() * Vector4d(0.0f, 0.0f, -1.0f, 0.0f)).head<3>();
    }
    return -Vector3d::UnitZ();
}

Matrix4d SceneCamera::GetViewMatrix() const { return m_viewMat; }

Matrix4d SceneCamera::GetViewProjectionMatrix() const { return m_viewProjectionMat; }

void SceneCamera::CalculateViewProjectionMatrix() {
    // CalculateViewMatrix
    {
        if (m_direct_lookat) {
            m_viewMat = LookAt(m_eye, m_center, m_up_vec);
            if (m_reflect_y0) {
                // Planar reflection about Y=0: V_refl = V_main * R
                // where R = diag(1, -1, 1, 1).  This ensures floor points
                // (Y=0) project to the SAME screen-space UV as the main
                // camera, so the screen-UV reflection texture lookup aligns.
                m_viewMat.col(1) *= -1.0;
            }
        } else if (m_node) {
            Affine3d nodeTrans(m_node->GetLocalTrans());
            Vector3d eye    = nodeTrans * Vector3d::Zero();
            Vector3d center = nodeTrans * (-Vector3d::UnitZ());
            Vector3d up     = Vector3d::UnitY();
            m_viewMat       = LookAt(eye, center, up);
        } else
            m_viewMat = Matrix4d::Identity();
    };

    if (m_perspective) {
        m_viewProjectionMat =
            Perspective(Radians(m_fov), m_aspect, m_nearClip, m_farClip) * m_viewMat;
    } else {
        double left         = -m_width / 2.0f;
        double right        = m_width / 2.0f;
        double bottom       = -m_height / 2.0f;
        double up           = m_height / 2.0f;
        m_viewProjectionMat = Ortho(left, right, bottom, up, m_nearClip, m_farClip) * m_viewMat;
    }

    if (m_reflect_y0) {
        // Negate VP row 1 (Y) to counteract the Vulkan gl_Position.y *= -1
        // injected by shader preprocessing.  This makes the reflection texture
        // store content with "un-flipped" Y.  The floor shader's screenUV is
        // computed from the Y-flipped main camera, so its Y is inverted —
        // sampling the un-flipped reflection texture with inverted Y produces
        // the correct mirror lookup.
        m_viewProjectionMat.row(1) *= -1.0;
    }
}

void SceneCamera::Update() { CalculateViewProjectionMatrix(); }

void SceneCamera::AttatchNode(std::shared_ptr<SceneNode> node) {
    if (! node) {
        LOG_ERROR("Attach a null node to camera");
        return;
    }
    m_node = node;
    Update();
}

void SceneCamera::LoadPaths(std::vector<CameraPath> paths) {
    m_paths       = std::move(paths);
    m_currentPath = 0;
    m_pathTime    = 0;
    m_fading      = false;
    m_fadeTime    = 0;
    if (! m_paths.empty() && m_paths[0].keyframes.size() >= 1) {
        auto& kf = m_paths[0].keyframes[0];
        SetDirectLookAt(kf.eye, kf.center, kf.up);
    }
}

void SceneCamera::AdvanceTime(double dt) {
    if (m_paths.empty()) return;

    m_pathTime += dt;
    auto& path = m_paths[m_currentPath];

    // Move to next path when current one finishes
    if (m_pathTime >= path.duration) {
        // Capture current camera position for crossfade
        if (m_fadeEnabled) {
            m_fadeFromEye    = m_eye;
            m_fadeFromCenter = m_center;
            m_fadeFromUp     = m_up_vec;
            m_fading         = true;
            m_fadeTime       = 0;
        }

        m_pathTime -= path.duration;
        m_currentPath   = (m_currentPath + 1) % m_paths.size();
        auto& next_path = m_paths[m_currentPath];
        // Clamp in case dt was very large
        if (m_pathTime >= next_path.duration) m_pathTime = 0;
    }

    auto& cur = m_paths[m_currentPath];
    if (cur.keyframes.size() < 2) return;

    // Find the two keyframes surrounding the current time
    auto& kf0 = cur.keyframes[0];
    auto& kf1 = cur.keyframes[1];

    double t = (cur.duration > 0) ? m_pathTime / cur.duration : 0;
    t        = std::clamp(t, 0.0, 1.0);

    Vector3d eye    = kf0.eye + t * (kf1.eye - kf0.eye);
    Vector3d center = kf0.center + t * (kf1.center - kf0.center);
    Vector3d up     = (kf0.up + t * (kf1.up - kf0.up)).normalized();

    // Crossfade: blend from previous path's end position to new path
    if (m_fading) {
        m_fadeTime += dt;
        double ft = std::clamp(m_fadeTime / m_fadeDuration, 0.0, 1.0);
        if (ft < 1.0) {
            double alpha = ft * ft * (3.0 - 2.0 * ft); // smoothstep
            eye          = m_fadeFromEye + alpha * (eye - m_fadeFromEye);
            center       = m_fadeFromCenter + alpha * (center - m_fadeFromCenter);
            up           = (m_fadeFromUp + alpha * (up - m_fadeFromUp)).normalized();
        } else {
            m_fading = false;
        }
    }

    SetDirectLookAt(eye, center, up);
}