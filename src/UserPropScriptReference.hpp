#pragma once

// Whether a script anywhere in the parsed scene reads a given user property.
//
// userPropHasRuntimeBinding (UserPropRuntimeUpdate.hpp) answers a narrower
// question: can an edited property be pushed onto the live scene without a
// reparse. It deliberately excludes scripts, because a script's
// scriptProperties are seeded once when the script compiles and never
// re-seeded, so even a property a script reads needs a full reload to pick
// up a new value. This header answers a different question: does anything in
// the scene use this property at all. That's what the settings-panel
// bindings sidecar wants — a property a script reads is a live, working
// control, it just happens to need a reload to update.
//
// Two ways a script can reference a property, most confident first:
//   1. Structured: a scriptproperties entry shaped like
//      {"somePropName": {"user": "<name>", ...}}. WE's own
//      createScriptProperties builder (qml_helper/PropertyScriptDispatchJs.hpp)
//      resolves this through engine.userProperties.<name> when the script
//      initializes, so a match here is exact.
//   2. Textual: the property name appears literally in the script's own JS
//      source, e.g. `engine.userProperties.foo` or
//      `engine.userProperties['foo']`. There's no JS parser here, so this is
//      a plain substring search — deliberately loose. A false positive (some
//      unrelated identifier happens to contain the property name) leaves a
//      working control looking bound, which is the same outcome the code
//      before this change produced for every declared property. A false
//      negative would gray out a control the wallpaper genuinely uses, which
//      is the failure this function exists to avoid.

#include "Scene/Scene.h"

#include <nlohmann/json.hpp>
#include <string>

namespace wek
{

using wallpaper::Scene;

namespace detail
{

inline bool scriptReferencesUserProp(const std::string& script,
                                     const std::string& scriptPropertiesJson,
                                     const std::string& name) {
    if (script.find(name) != std::string::npos) return true;

    if (scriptPropertiesJson.empty()) return false;
    auto parsed = nlohmann::json::parse(scriptPropertiesJson, nullptr, /*allow_exceptions=*/false);
    if (! parsed.is_object()) return false;
    for (const auto& [key, val] : parsed.items()) {
        (void)key;
        if (val.is_object() && val.contains("user") && val.at("user").is_string() &&
            val.at("user").get<std::string>() == name)
            return true;
    }
    return false;
}

} // namespace detail

inline bool userPropReferencedInScript(const Scene& scene, const std::string& name) {
    for (const auto& s : scene.propertyScripts)
        if (detail::scriptReferencesUserProp(s.script, s.scriptProperties, name)) return true;
    for (const auto& s : scene.colorScripts)
        if (detail::scriptReferencesUserProp(s.script, s.scriptProperties, name)) return true;
    for (const auto& s : scene.shaderValueScripts)
        if (detail::scriptReferencesUserProp(s.script, s.scriptProperties, name)) return true;
    for (const auto& s : scene.soundVolumeScripts)
        if (detail::scriptReferencesUserProp(s.script, s.scriptProperties, name)) return true;
    for (const auto& tl : scene.textLayers)
        if (detail::scriptReferencesUserProp(tl.script, tl.scriptProperties, name)) return true;
    return false;
}

} // namespace wek
