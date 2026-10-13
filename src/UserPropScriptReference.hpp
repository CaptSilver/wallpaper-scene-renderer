#pragma once

// Whether a script anywhere in the parsed scene reads a given user property.
//
// userPropHasRuntimeBinding (UserPropRuntimeUpdate.hpp) answers a narrower
// question: can an edited property be pushed onto the live scene without a
// reparse. It deliberately excludes scripts, because a script's
// scriptProperties are seeded once when the script compiles and never
// re-seeded, so even a property a script reads needs a full reload to pick
// up a new value. The parser reports those structured bindings as static uses
// (recordStructuredScriptUserProps), which is what makes the reload happen.
// This header also answers the broader question the settings-panel bindings
// sidecar asks: does anything in the scene use this property at all. A
// property a script reads is a live, working control, it just happens to need
// a reload to update.
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
//      a plain substring search, deliberately loose. A false positive (some
//      unrelated identifier happens to contain the property name) leaves a
//      working control looking bound. A false negative would gray out a
//      control the wallpaper genuinely uses, which is the failure this
//      function exists to avoid. Textual reads are not counted as static
//      uses: they usually sit in update(), which the live property refresh
//      does feed, so forcing a reload for them would regress those scenes.

#include "Scene/Scene.h"
#include "UserPropUseRecorder.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace wek
{

using wallpaper::Scene;

// The user properties a script's stored scriptproperties block binds with
// {"<key>": {"user": "<name>", ...}}. Only the string form of "user" counts:
// it is the one createScriptProperties resolves (the conditional object form
// is not honored by the script runtime). Malformed JSON yields nothing.
inline std::vector<std::string> structuredScriptUserProps(const std::string& scriptPropertiesJson) {
    std::vector<std::string> names;
    if (scriptPropertiesJson.empty()) return names;
    auto parsed = nlohmann::json::parse(scriptPropertiesJson, nullptr, /*allow_exceptions=*/false);
    if (! parsed.is_object()) return names;
    for (const auto& [key, val] : parsed.items()) {
        (void)key;
        if (val.is_object() && val.contains("user") && val.at("user").is_string())
            names.push_back(val.at("user").get<std::string>());
    }
    return names;
}

namespace detail
{

inline bool scriptReferencesUserProp(const std::string& script,
                                     const std::string& scriptPropertiesJson,
                                     const std::string& name) {
    if (script.find(name) != std::string::npos) return true;
    for (const auto& structured : structuredScriptUserProps(scriptPropertiesJson))
        if (structured == name) return true;
    return false;
}

} // namespace detail

// Reports, to the active UserPropUseRecorder, every property a script in the
// scene binds through its stored scriptproperties.
inline void recordStructuredScriptUserProps(const Scene& scene) {
    auto record = [](const std::string& scriptProperties) {
        for (const auto& name : structuredScriptUserProps(scriptProperties))
            wallpaper::recordUserPropStaticUse(name, "scriptproperties");
    };
    for (const auto& s : scene.propertyScripts) record(s.scriptProperties);
    for (const auto& s : scene.colorScripts) record(s.scriptProperties);
    for (const auto& s : scene.shaderValueScripts) record(s.scriptProperties);
    for (const auto& s : scene.soundVolumeScripts) record(s.scriptProperties);
    for (const auto& tl : scene.textLayers) record(tl.scriptProperties);
}

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
