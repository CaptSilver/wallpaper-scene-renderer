#pragma once
#include <cstdint>
#include <array>
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

    // Light kind discriminator.  Default `Point` preserves pre-volumetric
    // behavior for tests + scenes whose JSON omits the `light` string.  Modern
    // workshop scenes serialize one of `lpoint` / `lspot` / `ltube` /
    // `ldirectional` (the editor sprite filenames); the legacy `point` value
    // remains the catch-all for unknown strings.
    enum class LightKind : uint8_t {
        Point,
        LPoint,
        LSpot,
        LTube,
        LDirectional,
    };

    LightKind kind() const { return m_kind; }
    void      setKind(LightKind k) { m_kind = k; }

    // Volumetric-leg fields parsed from light JSON object's
    // `castvolumetrics`/`density`/`volumetricsexponent`.  `_explicit` flag
    // distinguishes "JSON authored castvolumetrics" (either true OR false —
    // either form was intentional) from "JSON omitted the field" (heuristic
    // applies via density>0).  Density=0 with no explicit flag means the
    // light does not participate in volumetrics; density>0 with no explicit
    // flag opts in.
    struct VolumetricParams {
        bool  cast_volumetrics_explicit { false };
        bool  cast_volumetrics_value    { false };
        float density                   { 0.0f };
        float exponent                  { 1.0f };
    };

    const VolumetricParams& volumetric() const { return m_vol; }
    void setVolumetric(const VolumetricParams& v) { m_vol = v; }

    bool                        castShadow() const { return m_cast_shadow; }
    void                        setCastShadow(bool b) { m_cast_shadow = b; }
    const std::array<float, 3>& cascadeDistances() const { return m_cascade_distances; }
    void setCascadeDistances(const std::array<float, 3>& d) { m_cascade_distances = d; }

    // Returns true when this light should contribute to the volumetric chain
    // this frame.  Composes the heuristic (density > 0 implicitly opts in
    // unless `castvolumetrics:false` was authored) with the `WEKDE_VOLUMETRICS`
    // env override (force-off shadows everything; force-on still requires
    // density > 0 since a zero-density pass writes a zero buffer).  Body in
    // SceneLight.cpp to keep the env-read TU-local.
    bool castsVolumetrics() const;

    // Re-reads WEKDE_VOLUMETRICS from the current process environment and
    // updates the TU-local override cache.  Test-only — the env is normally
    // read once at module init.  Safe to call from doctest cases between
    // setenv/unsetenv pairs; not thread-safe and not meant for production
    // toggling.
    static void _resetVolumetricsOverrideForTesting();

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

    LightKind m_kind { LightKind::Point };
    VolumetricParams m_vol {};
    bool                 m_cast_shadow { false };
    std::array<float, 3> m_cascade_distances { 0.0f, 100.0f, 200.0f };
};
} // namespace wallpaper
