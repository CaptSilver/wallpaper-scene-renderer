#pragma once

// Deciding whether a user-property change can be applied to the live scene.
//
// The parser records two kinds of runtime binding: a layer's `visible` field,
// re-resolved against the property, and a shader uniform fed by it.  Text
// layers additionally re-rasterise when their pointsize property moves.
// Everything else a property feeds is baked in at parse time — most commonly a
// property script's scriptProperties, which are seeded once when the script is
// compiled and never re-seeded — so a change there only takes effect if the
// scene is parsed again.
//
// Header-only so the decision can be exercised without the wallpaper bridge.

#include "Scene/Scene.h"
#include "WPUserProperties.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace wek
{

using wallpaper::Scene;
using wallpaper::WPUserProperties;

// Names in `props` whose value differs from the one `current` holds, plus any
// name `current` has never seen.
inline std::vector<std::string> changedUserProps(const WPUserProperties& current,
                                                 const nlohmann::json&   props) {
    std::vector<std::string> changed;
    if (! props.is_object()) return changed;
    for (auto it = props.begin(); it != props.end(); ++it) {
        auto known = current.GetProperty(it.key());
        if (! known.has_value() || *known != it.value()) changed.push_back(it.key());
    }
    return changed;
}

inline bool userPropHasRuntimeBinding(const Scene& scene, const std::string& name) {
    if (scene.userPropVisBindings.count(name) != 0) return true;
    if (scene.userPropUniformBindings.count(name) != 0) return true;
    for (const auto& tl : scene.textLayers) {
        if (tl.pointsizeUserProp == name) return true;
    }
    return false;
}

// The subset of `changed` that nothing in the live scene can apply, i.e. the
// reason to reload.
inline std::vector<std::string> userPropsNeedingReload(const Scene&                    scene,
                                                       const std::vector<std::string>& changed) {
    std::vector<std::string> needReload;
    for (const auto& name : changed) {
        if (! userPropHasRuntimeBinding(scene, name)) needReload.push_back(name);
    }
    return needReload;
}

} // namespace wek
