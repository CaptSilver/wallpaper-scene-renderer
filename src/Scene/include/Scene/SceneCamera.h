#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <Eigen/Dense>
#include "SceneImageEffectLayer.h"

namespace wallpaper
{

class SceneNode;

struct CameraKeyframe {
    Eigen::Vector3d eye;
    Eigen::Vector3d center;
    Eigen::Vector3d up;
    double          timestamp { 0 };
};

struct CameraPath {
    double                      duration { 0 };
    std::vector<CameraKeyframe> keyframes;
};

class SceneCamera {
public:
    explicit SceneCamera(i32 width, i32 height, float near, float far)
        : m_width(width),
          m_height(height),
          m_aspect(m_width / m_height),
          m_nearClip(near),
          m_farClip(far),
          m_perspective(false) {}

    explicit SceneCamera(float aspect, float near, float far, float fov)
        : m_aspect(aspect), m_nearClip(near), m_farClip(far), m_fov(fov), m_perspective(true) {}

    SceneCamera(const SceneCamera&) = default;

    void Update();

    void AttatchNode(std::shared_ptr<SceneNode>);

    bool   IsPerspective() const { return m_perspective; }
    double Aspect() const { return m_aspect; }
    double Width() const { return m_width; }
    double Height() const { return m_height; }
    double NearClip() const { return m_nearClip; }
    double FarClip() const { return m_farClip; }
    double Fov() const { return m_fov; }

    void SetWidth(double value) {
        m_width  = value;
        m_aspect = m_width / m_height;
    }
    void SetHeight(double value) {
        m_height = value;
        m_aspect = m_width / m_height;
    }
    void SetAspect(double aspect) { m_aspect = aspect; }
    void SetFov(double value) { m_fov = value; }

    void  AttatchImgEffect(std::shared_ptr<SceneImageEffectLayer> eff) { m_imgEffect = eff; }
    bool  HasImgEffect() const { return (bool)m_imgEffect; }
    auto& GetImgEffect() { return m_imgEffect; }

    void SetDirectLookAt(const Eigen::Vector3d& eye, const Eigen::Vector3d& center,
                         const Eigen::Vector3d& up);

    Eigen::Vector3d GetPosition() const;
    Eigen::Vector3d GetDirection() const;
    Eigen::Vector3d GetEye() const { return m_eye; }
    Eigen::Vector3d GetCenter() const { return m_center; }
    Eigen::Vector3d GetUp() const { return m_up_vec; }

    Eigen::Matrix4d GetViewMatrix() const;
    Eigen::Matrix4d GetViewProjectionMatrix() const;

    std::shared_ptr<SceneNode> GetAttachedNode() const { return m_node; }

    void Clone(const SceneCamera& cam) {
        m_width       = cam.m_width;
        m_height      = cam.m_height;
        m_aspect      = cam.m_aspect;
        m_nearClip    = cam.m_nearClip;
        m_farClip     = cam.m_farClip;
        m_perspective = cam.m_perspective;
    }

    void LoadPaths(std::vector<CameraPath> paths);
    void AdvanceTime(double dt);
    bool HasPaths() const { return !m_paths.empty(); }
    const std::vector<CameraPath>& GetPaths() const { return m_paths; }

    // Planar reflection about Y=0: negates eye.y, center.y, up.y in VP matrix
    void SetReflectY0(bool v) { m_reflect_y0 = v; }
    bool IsReflectY0() const { return m_reflect_y0; }

private:
    void CalculateViewProjectionMatrix();

    double m_width { 1.0f };
    double m_height { 1.0f };
    double m_aspect { 16.0f / 9.0f };
    double m_nearClip { 0.01f };
    double m_farClip { 1000.0f };
    double m_fov { 45.0f };
    bool   m_perspective;

    Eigen::Matrix4d m_viewMat { Eigen::Matrix4d::Identity() };
    Eigen::Matrix4d m_viewProjectionMat { Eigen::Matrix4d::Identity() };

    bool            m_direct_lookat { false };
    Eigen::Vector3d m_eye { Eigen::Vector3d::Zero() };
    Eigen::Vector3d m_center { -Eigen::Vector3d::UnitZ() };
    Eigen::Vector3d m_up_vec { Eigen::Vector3d::UnitY() };

    std::shared_ptr<SceneNode>             m_node;
    std::shared_ptr<SceneImageEffectLayer> m_imgEffect { nullptr };

    // Camera path animation
    std::vector<CameraPath> m_paths;
    size_t                  m_currentPath { 0 };
    double                  m_pathTime { 0 };

    // Planar reflection about Y=0
    bool m_reflect_y0 { false };
};
} // namespace wallpaper
