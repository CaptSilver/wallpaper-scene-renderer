#pragma once

// Writing a layer's alpha into its materials.
//
// Wallpaper Engine's shaders don't agree on where layer opacity lives.  Only
// genericimage and genericimage2 declare `g_UserAlpha` at all, and
// genericimage2 hides it behind `#ifndef VERSION` — the text path compiles
// with VERSION=1, so text falls back to `g_Color4.a`, as does genericimage4.
// flat (solidlayer) reads a standalone `g_Alpha`.  A constValue whose name the
// compiled shader doesn't declare is dropped on upload, so writing one name
// reaches only some layers and the rest ignore the animation entirely.  Write
// all of them and let the shader pick.
//
// Header-only so the write can be exercised without the render bridge.

#include "Scene/Scene.h"
#include "Scene/SceneImageEffectLayer.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneNode.h"
#include "Type.hpp"

namespace wek
{

using wallpaper::i32;
using wallpaper::Scene;
using wallpaper::SceneMaterial;
using wallpaper::SceneNode;

// `writeFlatAlpha` covers flat.frag's standalone g_Alpha.  Pass false for the
// base pass of a layer that feeds an effect chain: a solidlayer's flat quad is
// parsed transparent on purpose so the chain's first pingpong starts from
// (0,0,0,0), and raising it paints a coloured rectangle under the effect output.
inline void writeMaterialAlpha(SceneMaterial* mat, float alpha, bool writeFlatAlpha = true) {
    if (mat == nullptr) return;
    auto& constValues          = mat->customShader.constValues;
    constValues["g_UserAlpha"] = std::vector<float> { alpha };
    auto it                    = constValues.find("g_Color4");
    if (it != constValues.end() && it->second.size() >= 4) {
        it->second[3] = alpha;
    }
    if (writeFlatAlpha) constValues["g_Alpha"] = std::vector<float> { alpha };
    mat->customShader.constValuesDirty = true;
}

inline void writeNodeAlpha(Scene& scene, SceneNode* sourceNode, i32 nodeId, float alpha) {
    auto       eit      = scene.nodeEffectLayerMap.find(nodeId);
    const bool hasChain = eit != scene.nodeEffectLayerMap.end() && eit->second != nullptr;

    if (sourceNode != nullptr && sourceNode->HasMaterial()) {
        writeMaterialAlpha(sourceNode->Mesh()->Material(), alpha, ! hasChain);
    }
    if (! hasChain) return;

    auto* eff = eit->second;
    for (std::size_t i = 0; i < eff->EffectCount(); i++) {
        auto& e = eff->GetEffect(i);
        for (auto& en : e->nodes) {
            if (en.sceneNode && en.sceneNode->HasMaterial()) {
                writeMaterialAlpha(en.sceneNode->Mesh()->Material(), alpha);
            }
        }
    }
    writeMaterialAlpha(eff->FinalMesh().Material(), alpha);
}

} // namespace wek
