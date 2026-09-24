#pragma once

// Reconciling parse-time effect-visibility bindings against a
// SceneImageEffectLayer's final, post-compaction state.
//
// assembleEffects records a *pending* binding the instant it builds a
// user-property-bound effect -- but SceneImageEffectLayer::RemoveFailedEffects
// (run once, after every object in the scene has parsed) can still erase an
// EARLIER effect on the same layer whose shader never compiled, shifting
// every later effect's position in m_effects down by one. A binding keyed
// on the position captured at build time would silently point at the wrong
// effect after that erase. Resolving by object identity instead of a
// captured index sidesteps the whole class of bug: whatever position the
// bound SceneImageEffect actually ends up at, that's the index recorded.
//
// Also excludes whichever effect ends up LAST on its layer: ResolveEffect
// (SceneImageEffectLayer.cpp) picks the structurally-last effect as the
// chain's terminal write target without looking at visibility, and that
// write is forced to keep its render target's CLEAR even when hidden
// (CustomShaderPass.hpp's IsHiddenPassSkippable) -- so toggling the last
// effect off risks clearing the whole composite instead of just hiding one
// effect's contribution. Nothing in this codebase currently proves that's
// safe, so the last position keeps today's full-reload behaviour.

#include "Scene/Scene.h"
#include "Scene/SceneImageEffectLayer.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace wek
{

using wallpaper::i32;
using wallpaper::Scene;
using wallpaper::SceneImageEffect;
using wallpaper::SceneImageEffectLayer;

// One user-bound effect visibility captured the moment assembleEffects built
// it -- by identity (a raw pointer into the still-being-built layer), not
// yet a fixed position.
struct PendingUserPropEffectVis {
    SceneImageEffect* effect { nullptr };
    i32               nodeId { 0 };
    bool              defaultVisible { true };
    std::string       rawVisibleJson;
    std::string       propName;
};

// Walks `pending`, looks each entry's layer up in `nodeEffectLayerMap`, and
// -- for every entry whose effect pointer is still present in that layer's
// CURRENT m_effects, and is not the last entry in it -- records its live
// index into `bindings`. An entry that was dropped by RemoveFailedEffects,
// whose node never made it into nodeEffectLayerMap, or that ended up last on
// its layer is silently skipped: no binding means
// userPropHasRuntimeBinding() returns false for that name, and the existing
// reload fallback covers it -- the same safe degrade an ordinary
// material-load failure already gets.
inline void resolvePendingUserPropEffectVis(
    const std::vector<PendingUserPropEffectVis>&           pending,
    const std::unordered_map<i32, SceneImageEffectLayer*>& nodeEffectLayerMap,
    std::unordered_map<std::string, std::vector<Scene::UserPropEffectVisibility>>& bindings) {
    for (const auto& p : pending) {
        auto it = nodeEffectLayerMap.find(p.nodeId);
        if (it == nodeEffectLayerMap.end() || ! it->second) continue;
        auto* layer = it->second;
        for (std::size_t i = 0; i < layer->EffectCount(); i++) {
            if (layer->GetEffect(i).get() != p.effect) continue;
            if (i + 1 == layer->EffectCount()) break; // last on its layer -- leave unregistered
            bindings[p.propName].push_back(
                { p.nodeId, (i32)i, p.defaultVisible, p.rawVisibleJson });
            break;
        }
    }
}

} // namespace wek
