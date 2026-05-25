#include "Scene/SceneLight.hpp"

namespace wallpaper
{

// Volumetric env override.  Wired into a real getenv() read in a follow-up
// commit; until then the `Auto` default lets the heuristic run unmodified.
namespace
{
enum class VolumetricsOverride : uint8_t
{
    Auto     = 0,
    ForceOn  = 1,
    ForceOff = 2,
};
VolumetricsOverride g_volumetricsOverride { VolumetricsOverride::Auto };
} // namespace

bool SceneLight::castsVolumetrics() const {
    // Env override applies first so force-off acts as an unconditional
    // kill-switch.  force-on still requires density>0 — a zero-density pass
    // writes a zero buffer, so emitting it is wasted GPU work.
    switch (g_volumetricsOverride) {
    case VolumetricsOverride::ForceOff: return false;
    case VolumetricsOverride::ForceOn: return m_vol.density > 0.0f;
    case VolumetricsOverride::Auto: break;
    }
    if (m_vol.cast_volumetrics_explicit) {
        return m_vol.cast_volumetrics_value && m_vol.density > 0.0f;
    }
    return m_vol.density > 0.0f;
}

} // namespace wallpaper
