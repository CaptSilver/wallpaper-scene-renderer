#pragma once

#include "Scene/SceneLight.hpp"

#include <string_view>

namespace wallpaper
{

// Map the WE light JSON `light` string (the kind discriminator) to our
// SceneLight::LightKind enum.  Unknown / empty strings fall back to
// LightKind::Point — workshop scenes occasionally author future-kind values
// our parser doesn't recognize; silently falling back keeps the scene loading.
// Comparison is case-sensitive: WE's editor always emits lowercase kind
// names ("lpoint" / "lspot" / "ltube" / "ldirectional" / legacy "point").
inline SceneLight::LightKind parseLightKind(std::string_view s) {
    if (s == "lpoint")       return SceneLight::LightKind::LPoint;
    if (s == "lspot")        return SceneLight::LightKind::LSpot;
    if (s == "ltube")        return SceneLight::LightKind::LTube;
    if (s == "ldirectional") return SceneLight::LightKind::LDirectional;
    return SceneLight::LightKind::Point;
}

} // namespace wallpaper
