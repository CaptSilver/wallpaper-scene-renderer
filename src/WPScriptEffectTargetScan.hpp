#pragma once

// Finds the layers whose effects a SceneScript can hide or show at runtime
// with getEffect(...).visible = ..., from the script text alone.
//
// The render graph only makes an effect pass hideable (able to draw its input
// through while hidden) when the parser registered a visibility binding for
// it. A script that flips an unbound effect gets no such registration, so the
// hidden pass keeps its buffer clear, draws nothing, and blanks the layer.
// This scan names the layers such a script can reach so every effect of those
// layers can be registered. It is deliberately coarse: scripts address effects
// by index in loops and by name through maps, so guessing which effect is
// toggled is less reliable than tagging the whole layer.
//
// A script is a candidate when it contains a getEffect( call and a `.visible`
// assignment (a comparison does not count). For a candidate:
//   - ownLayer: the script's own object, when the effect receiver is thisLayer
//     or thisObject, or when it is a plain variable and nothing names another
//     layer (`var self = thisLayer; self.getEffect(0)...`). A script that only
//     names other layers by literal does not target its host.
//   - layerNames: every string-literal getLayer('X') / getLayer("X"). Matched
//     by name, so every object sharing the name is a target.
//   - unresolved: an effect receiver that is neither thisLayer/thisObject nor
//     a literal getLayer chain (an indexed receiver such as `kids[i]` counts as
//     neither), in a script that also reaches layers
//     dynamically (getLayer(<expression>), getChildren, getParent,
//     createLayer). Nothing can be said about which layer it hits, so the whole
//     scene's layers are treated as targets.
//
// Known gap: an effect handle stored by one script and written by another
// (`shared.e = thisLayer.getEffect(0)` here, `shared.e.visible = false`
// there) is a candidate in neither script.

#include "Core/Literals.hpp"
#include "Utils/Logging.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace wek
{

using wallpaper::i32;
using wallpaper::usize;

struct ScriptEffectVisTargets {
    bool                     ownLayer { false };
    std::vector<std::string> layerNames;
    bool                     unresolved { false };
};

// nullopt when `script` cannot toggle an effect's visibility.
inline std::optional<ScriptEffectVisTargets> scanScriptEffectVisibility(std::string_view script) {
    static const std::regex getEffectRe(R"(getEffect\s*\()");
    static const std::regex visibleWriteRe(R"(\.visible\s*=(?!=))");
    static const std::regex receiverRe(R"((\w+|\)|\])\s*\.\s*getEffect\s*\()");
    static const std::regex getLayerLiteralRe(R"(getLayer\s*\(\s*['"]([^'"]+)['"]\s*\))");
    static const std::regex getLayerDynamicRe(R"(getLayer\s*\(\s*(?!['"])[^)\s])");
    static const std::regex dynamicReachRe(R"(\b(getChildren|getParent|createLayer)\s*\()");
    static const std::regex literalChainRe(
        R"(getLayer\s*\(\s*['"][^'"]+['"]\s*\)\s*\.\s*getEffect\s*\()");

    const std::string s(script);
    if (! std::regex_search(s, getEffectRe) || ! std::regex_search(s, visibleWriteRe))
        return std::nullopt;

    std::set<std::string> receivers;
    usize                 closeParenReceivers = 0;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), receiverRe);
         it != std::sregex_iterator();
         ++it) {
        receivers.insert((*it)[1].str());
        if ((*it)[1].str() == ")") closeParenReceivers++;
    }

    ScriptEffectVisTargets out;
    std::set<std::string>  seen;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), getLayerLiteralRe);
         it != std::sregex_iterator();
         ++it) {
        if (seen.insert((*it)[1].str()).second) out.layerNames.push_back((*it)[1].str());
    }

    const bool dynamic =
        std::regex_search(s, getLayerDynamicRe) || std::regex_search(s, dynamicReachRe);

    bool                  thisReceiver = false;
    bool                  bareOnly     = true;
    std::set<std::string> others;
    for (const auto& r : receivers) {
        if (r == "thisLayer" || r == "thisObject") {
            thisReceiver = true;
            continue;
        }
        others.insert(r);
        if (r == ")" || r == "]") bareOnly = false;
    }

    // `getLayer('X').getEffect(` for every ')' receiver: the layer is named,
    // so the receiver is not unresolved even in a script that looks layers up
    // dynamically elsewhere.
    usize literalChains = 0;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), literalChainRe);
         it != std::sregex_iterator();
         ++it)
        literalChains++;
    const bool literalChainOnly =
        others == std::set<std::string> { ")" } && literalChains == closeParenReceivers;

    out.unresolved = ! others.empty() && ! literalChainOnly && dynamic;
    out.ownLayer   = thisReceiver || (out.layerNames.empty() && ! dynamic && bareOnly);
    return out;
}

// What the parser needs at layer-assembly time: ids for scripts that target
// their own object, names for scripts that name a layer, and the scene-wide
// fallback for scripts whose target cannot be determined.
struct ScriptEffectTargetSet {
    std::unordered_set<i32>         ids;
    std::unordered_set<std::string> names;
    // Set by an unresolved script. contains() then holds for every layer that
    // reaches assembleEffects (text layers and synthesized compose passthroughs
    // included), not only for objects with authored effects.
    bool fallback { false };

    bool contains(i32 id, const std::string& name) const {
        return fallback || ids.count(id) != 0 || names.count(name) != 0;
    }
};

namespace detail
{
inline void gatherScripts(const nlohmann::json& j, std::vector<std::string>& out) {
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "script" && it.value().is_string())
                out.push_back(it.value().get<std::string>());
            else
                gatherScripts(it.value(), out);
        }
    } else if (j.is_array()) {
        for (const auto& v : j) gatherScripts(v, out);
    }
}
} // namespace detail

// Scans every `script` string under each of sceneJson["objects"] (including an
// effect's own visible script) and records the layers they can toggle effects
// on.
inline void collectScriptEffectTargets(const nlohmann::json&  sceneJson,
                                       ScriptEffectTargetSet& out) {
    auto objs = sceneJson.find("objects");
    if (objs == sceneJson.end() || ! objs->is_array()) return;

    for (const auto& obj : *objs) {
        if (! obj.is_object()) continue;
        std::vector<std::string> scripts;
        detail::gatherScripts(obj, scripts);
        for (const auto& script : scripts) {
            auto t = scanScriptEffectVisibility(script);
            if (! t) continue;
            if (t->ownLayer && obj.contains("id") && obj["id"].is_number_integer())
                out.ids.insert(obj["id"].get<i32>());
            for (auto& n : t->layerNames) out.names.insert(n);
            if (t->unresolved) {
                out.fallback = true;
                // value() throws on a key of the wrong type; malformed but loadable
                // scenes must not abort the parse over a log line.
                const i32 id =
                    obj.contains("id") && obj["id"].is_number_integer() ? obj["id"].get<i32>() : -1;
                const std::string name = obj.contains("name") && obj["name"].is_string()
                                             ? obj["name"].get<std::string>()
                                             : std::string();
                LOG_INFO("script on object id=%d '%s' toggles effects on a layer it cannot name; "
                         "every layer's effects are treated as script-toggled",
                         id,
                         name.c_str());
            }
        }
    }
}

} // namespace wek
