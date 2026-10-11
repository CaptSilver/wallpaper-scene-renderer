#pragma once

// Same identity-vs-index and terminal-position hazards as
// UserPropEffectVisResolve.hpp (read that file's header comment first) --
// this is the sibling resolver for effect-level visible.script bindings
// instead of visible.user ones. It feeds Scene::propertyScripts (the same
// vector object/animation-layer-attached scripts land in) rather than
// Scene::userPropEffectVisBindings, since a script-bound effect is dispatched
// through the SceneScript engine, not the runtime user-property apply path.

#include "Scene/Scene.h"
#include "Scene/SceneImageEffectLayer.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace wek
{

using wallpaper::i32;
using wallpaper::SceneImageEffect;
using wallpaper::SceneImageEffectLayer;
using wallpaper::ScenePropertyScript;

// One script-bound effect visibility captured the moment assembleEffects
// built it -- by identity (a raw pointer into the still-being-built layer),
// not yet a fixed position.
struct PendingScriptEffectVis {
    SceneImageEffect* effect { nullptr };
    i32               nodeId { 0 };
    std::string       layerName;
    std::string       script;
    std::string       scriptProperties;
    bool              initialVisible { true };
};

// Walks `pending`, looks each entry's layer up in `nodeEffectLayerMap`, and
// -- for every entry whose effect pointer is still present in that layer's
// CURRENT m_effects -- appends a resolved ScenePropertyScript to `out`,
// including one that ends up last on its layer: the render side now draws a
// hidden terminal's passthrough (CustomShaderPass::hideable)
// instead of leaving the structural write forced visible, so the terminal
// position is safe to register like any other. An entry that was dropped by
// RemoveFailedEffects, or whose node never made it into nodeEffectLayerMap,
// is still silently skipped: leaving the effect unregistered means it keeps
// its static `value` visibility forever, the same safe degrade an ordinary
// material-load failure already gets.
inline void resolvePendingScriptEffectVis(
    const std::vector<PendingScriptEffectVis>&             pending,
    const std::unordered_map<i32, SceneImageEffectLayer*>& nodeEffectLayerMap,
    std::vector<ScenePropertyScript>&                      out) {
    for (const auto& p : pending) {
        auto it = nodeEffectLayerMap.find(p.nodeId);
        if (it == nodeEffectLayerMap.end() || ! it->second) continue;
        auto* layer = it->second;
        for (std::size_t i = 0; i < layer->EffectCount(); i++) {
            if (layer->GetEffect(i).get() != p.effect) continue;
            ScenePropertyScript sps;
            sps.id               = p.nodeId;
            sps.property         = "visible";
            sps.script           = p.script;
            sps.scriptProperties = p.scriptProperties;
            sps.layerName        = p.layerName;
            sps.initialVisible   = p.initialVisible;
            sps.attachment       = ScenePropertyScript::Attachment::Effect;
            sps.effectIndex      = (i32)i;
            out.push_back(std::move(sps));
            break;
        }
    }
}

} // namespace wek
