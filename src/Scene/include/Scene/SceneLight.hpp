#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <Eigen/Dense>
#include "SceneNode.h"

namespace wallpaper
{

class SceneLight {
public:
    // `exponent` controls the falloff power consumed by ComputePBRLightShadow
    // as `radiance = color * pow(saturate(1 - d/radius), exponent)`.  Default
    // 1.0 (linear) preserves pre-2026-05 behavior for callers/tests that don't
    // supply an exponent.  Real-Time Earth (3557068717) authors 0.1 for a soft
    // long-tail falloff that keeps most of the radius lit.
    SceneLight(Eigen::Vector3f color, float radius, float intensity, float exponent = 1.0f)
        : m_color(color), m_radius(radius), m_intensity(intensity), m_exponent(exponent) {
        m_premultiplied_color = m_color * m_intensity * m_radius * m_radius;
    }
    ~SceneLight() = default;

    Eigen::Vector3f color() const { return m_color; }
    float           radius() const { return m_radius; }
    float           intensity() const { return m_intensity; }
    float           exponent() const { return m_exponent; }
    SceneNode*      node() const { return m_node.get(); }

    Eigen::Vector3f premultipliedColor() const { return m_premultiplied_color; }
    Eigen::Vector3f colorIntensity() const { return m_color * m_intensity; }

    void setNode(std::shared_ptr<SceneNode> node) { m_node = node; }

    void setColor(Eigen::Vector3f c) {
        m_color = c;
        recalcPremultiplied();
    }
    void setRadius(float r) {
        m_radius = r;
        recalcPremultiplied();
    }
    void setIntensity(float i) {
        m_intensity = i;
        recalcPremultiplied();
    }

private:
    void recalcPremultiplied() {
        m_premultiplied_color = m_color * m_intensity * m_radius * m_radius;
    }
    Eigen::Vector3f m_color { Eigen::Vector3f::Zero() };
    float           m_radius { 0.0f };
    float           m_intensity { 1.0f };
    float           m_exponent { 1.0f };

    Eigen::Vector3f            m_premultiplied_color { Eigen::Vector3f::Zero() };
    std::shared_ptr<SceneNode> m_node { nullptr };
};
} // namespace wallpaper
