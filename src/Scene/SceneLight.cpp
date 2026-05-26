#include "Scene/SceneLight.hpp"

#include <cstdlib>
#include <cstring>
#include <string_view>

namespace wallpaper
{

namespace
{
enum class VolumetricsOverride : uint8_t
{
    Auto     = 0,
    ForceOn  = 1,
    ForceOff = 2,
};

VolumetricsOverride parseEnvOverride() {
    const char* v = std::getenv("WEKDE_VOLUMETRICS");
    if (! v || *v == '\0') return VolumetricsOverride::Auto;
    std::string_view s { v };
    if (s == "force-off" || s == "0") return VolumetricsOverride::ForceOff;
    if (s == "force-on"  || s == "1") return VolumetricsOverride::ForceOn;
    if (s == "auto")                  return VolumetricsOverride::Auto;
    return VolumetricsOverride::Auto;
}

VolumetricsOverride g_volumetricsOverride { parseEnvOverride() };
} // namespace

void SceneLight::_resetVolumetricsOverrideForTesting() {
    g_volumetricsOverride = parseEnvOverride();
}

bool SceneLight::castsVolumetrics() const {
    switch (g_volumetricsOverride) {
    case VolumetricsOverride::ForceOff:
        return false;
    case VolumetricsOverride::ForceOn:
        return m_vol.density > 0.0f;
    case VolumetricsOverride::Auto:
        break;
    }
    if (m_vol.cast_volumetrics_explicit) {
        return m_vol.cast_volumetrics_value && m_vol.density > 0.0f;
    }
    return m_vol.density > 0.0f;
}

bool SceneLight::isVolumetricEmitterCandidate() const {
    if (! castsVolumetrics()) return false;
    return m_kind == LightKind::Point || m_kind == LightKind::LPoint;
}

} // namespace wallpaper
