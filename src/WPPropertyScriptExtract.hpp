#pragma once

// Extract property scripts from a single scene.json object.
//
// Top-level scripts (objects[N].{visible,origin,scale,angles,alpha}) attach
// to the object itself; per-rig scripts
// (objects[N].animationlayers[M].{...}) attach to a specific animation
// layer on a puppet rig — used by Lucy (3521337568) to offset start frames
// via thisObject.setFrame at init time.
//
// Header-only / pure helpers so they can be exercised directly by doctest
// without pulling in the full WPSceneParser machinery.

#include "Scene/Scene.h"
#include "Type.hpp"

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace wek {

using wallpaper::i32;
using wallpaper::ScenePropertyScript;

// Extract all property scripts defined directly on `host` (the `visible`,
// `origin`, `scale`, `angles`, `alpha` keys, each an object with a `script`
// child).  `attachment` + `animationLayerIndex` stamp the extracted
// ScenePropertyScript so the dispatcher can later rebind thisObject.
//
// Does NOT recurse into host.animationlayers — caller walks that array
// and invokes this function per element.  Keeping the walk outside the
// function makes the two attachment shapes testable independently.
inline void
extractPropertyScriptsFromHost(i32                                   id,
                               const std::string&                    layerName,
                               const nlohmann::json&                 host,
                               ScenePropertyScript::Attachment       attachment,
                               i32                                   animationLayerIndex,
                               std::vector<ScenePropertyScript>&     out) {
    for (const char* prop : { "visible", "origin", "scale", "angles", "alpha" }) {
        if (! host.contains(prop)) continue;
        auto& val = host.at(prop);
        if (! val.is_object() || ! val.contains("script")) continue;

        ScenePropertyScript sps;
        sps.id                  = id;
        sps.property            = prop;
        sps.script              = val.at("script").get<std::string>();
        sps.layerName           = layerName;
        sps.attachment          = attachment;
        sps.animationLayerIndex = animationLayerIndex;

        if (val.contains("scriptproperties"))
            sps.scriptProperties = val.at("scriptproperties").dump();

        if (std::string(prop) == "visible") {
            sps.initialVisible = val.value("value", true);
        } else if (std::string(prop) == "alpha") {
            sps.initialFloat = val.value("value", 1.0f);
        } else {
            if (val.contains("value") && val.at("value").is_string()) {
                std::istringstream iss(val.at("value").get<std::string>());
                iss >> sps.initialVec3[0] >> sps.initialVec3[1] >> sps.initialVec3[2];
            }
        }
        out.push_back(std::move(sps));
    }
}

// Walk objects[N].animationlayers[M] if present, extracting per-rig-layer
// scripts.  Called after `extractPropertyScriptsFromHost(... , Object, -1)`
// has handled the top-level scripts on the same object.
inline void
extractAnimationLayerScripts(i32                               id,
                             const std::string&                layerName,
                             const nlohmann::json&             obj,
                             std::vector<ScenePropertyScript>& out) {
    if (! obj.contains("animationlayers")) return;
    auto& alayers = obj.at("animationlayers");
    if (! alayers.is_array()) return;
    for (size_t alIdx = 0; alIdx < alayers.size(); alIdx++) {
        auto& al = alayers[alIdx];
        if (! al.is_object()) continue;
        extractPropertyScriptsFromHost(
            id, layerName, al,
            ScenePropertyScript::Attachment::AnimationLayer,
            static_cast<i32>(alIdx), out);
    }
}

} // namespace wek
