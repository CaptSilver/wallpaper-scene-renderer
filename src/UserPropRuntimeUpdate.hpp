#pragma once

// Deciding whether a user-property change can be applied to the live scene.
//
// The parser records three kinds of runtime binding: a layer's `visible` field,
// re-resolved against the property, and a shader uniform fed by it.  Text
// layers additionally re-rasterise when their pointsize property moves.
// Everything else a property feeds is baked in at parse time (an object's
// color, a texture variant, a property script's scriptProperties, which are
// seeded once when the script is compiled, ...), and the parser lists those in
// Scene::userPropStaticUses.  A property with any such use needs the scene
// parsed again, even if it also has a runtime binding.
//
// Header-only so the decision can be exercised without the wallpaper bridge.

#include "Scene/Scene.h"
#include "WPUserProperties.hpp"

#include <nlohmann/json.hpp>
#include <sstream>
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

// A resolved user-property JSON value, coerced to a layer's visible flag.
// bool passes through; a number is visible unless exactly 0; a string is
// visible unless empty, "0", or "false" (WE's own truthy-string convention);
// anything else (null, array, object) falls back to the layer's authored
// default.
inline bool ResolveVisibilityFromJson(const nlohmann::json& resolved, bool defaultVisible) {
    if (resolved.is_boolean()) return resolved.get<bool>();
    if (resolved.is_number()) return resolved.get<double>() != 0.0;
    if (resolved.is_string()) {
        auto s = resolved.get<std::string>();
        return ! s.empty() && s != "0" && s != "false";
    }
    return defaultVisible;
}

// A resolved user-property JSON value, coerced to the float vector a shader
// uniform binding wants: a bare number becomes one element, a string is
// whitespace-split into floats, an array keeps only its numeric elements.
inline std::vector<float> ParseFloatVectorFromJson(const nlohmann::json& val) {
    std::vector<float> floatVec;
    if (val.is_number()) {
        floatVec.push_back(val.get<float>());
    } else if (val.is_string()) {
        std::istringstream iss(val.get<std::string>());
        float              f;
        while (iss >> f) floatVec.push_back(f);
    } else if (val.is_array()) {
        for (const auto& elem : val) {
            if (elem.is_number()) floatVec.push_back(elem.get<float>());
        }
    }
    return floatVec;
}

inline bool userPropHasRuntimeBinding(const Scene& scene, const std::string& name) {
    if (scene.userPropVisBindings.count(name) != 0) return true;
    if (scene.userPropEffectVisBindings.count(name) != 0) return true;
    if (scene.userPropUniformBindings.count(name) != 0) return true;
    for (const auto& tl : scene.textLayers) {
        if (tl.pointsizeUserProp == name) return true;
    }
    return false;
}

// Whether parsing consumed the property into a field nothing re-reads live.
inline bool userPropHasStaticUse(const Scene& scene, const std::string& name) {
    auto it = scene.userPropStaticUses.find(name);
    return it != scene.userPropStaticUses.end() && ! it->second.empty();
}

// The fields a property was baked into, comma-joined, for the reload log.
inline std::string userPropStaticFields(const Scene& scene, const std::string& name) {
    std::string joined;
    auto        it = scene.userPropStaticUses.find(name);
    if (it == scene.userPropStaticUses.end()) return joined;
    for (const auto& field : it->second) {
        if (! joined.empty()) joined += ", ";
        joined += field;
    }
    return joined;
}

// The subset of `changed` that the live scene cannot apply in place, i.e. the
// reason to reload: nothing binds it at runtime, or parsing also baked it into
// some field. The second case matters because one property can feed both a
// live field and a parse-time one; pushing the new value to the live field
// alone would leave the other stale.
inline std::vector<std::string> userPropsNeedingReload(const Scene&                    scene,
                                                       const std::vector<std::string>& changed) {
    std::vector<std::string> needReload;
    for (const auto& name : changed) {
        if (! userPropHasRuntimeBinding(scene, name) || userPropHasStaticUse(scene, name))
            needReload.push_back(name);
    }
    return needReload;
}

} // namespace wek
