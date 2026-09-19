#pragma once
//
// Push a layer's live alpha / colour into the uniforms its shaders read.
//
// Image shaders (genericimage*) read the layer alpha from g_UserAlpha and the
// colour from g_Color4; WE's `flat` shader — every solid layer — reads the
// separate g_Alpha (float) and g_Color (vec3) instead.  Property animations
// and SceneScripts only ever updated the image-shader pair, so an animated or
// scripted solid layer kept the alpha and colour it was parsed with: SUBARU
// (3448290956) fades a full-screen black solid from 0 through 0.5 back to 0
// at load, and the frame stayed dimmed at the static 0.3 forever.
//
// The flat pair is written only on a layer's own base material.  Effect
// materials are seeded with the same base constants, but an effect shader
// may declare its own g_Alpha with a different meaning, so those keep the
// image-shader pair only.
//

#include "Scene/SceneMaterial.h"

#include <vector>

namespace wallpaper
{

// g_UserAlpha and g_Color4.a — what image shaders and effect passes read.
inline void writeLayerAlpha(SceneMaterial& mat, float alpha) {
    auto& cv          = mat.customShader.constValues;
    cv["g_UserAlpha"] = std::vector<float> { alpha };
    if (auto it = cv.find("g_Color4"); it != cv.end() && it->second.size() >= 4) {
        it->second[3] = alpha;
    }
    mat.customShader.constValuesDirty = true;
}

// g_Alpha — what the flat shader reads.  Only rewrites a slot the parser
// seeded, so materials without the uniform are left alone.
inline void writeFlatAlpha(SceneMaterial& mat, float alpha) {
    auto& cv = mat.customShader.constValues;
    if (auto it = cv.find("g_Alpha"); it != cv.end() && it->second.size() >= 1) {
        it->second[0]                     = alpha;
        mat.customShader.constValuesDirty = true;
    }
}

// g_Color4.rgb, keeping whatever alpha the slot already carries.
inline void writeLayerColor(SceneMaterial& mat, float r, float g, float b) {
    auto& cv    = mat.customShader.constValues;
    float alpha = 1.0f;
    if (auto it = cv.find("g_Color4"); it != cv.end() && it->second.size() >= 4) {
        alpha = it->second[3];
    }
    cv["g_Color4"]                    = std::vector<float> { r, g, b, alpha };
    mat.customShader.constValuesDirty = true;
}

// g_Color — the flat shader's colour, again only when the parser seeded it.
inline void writeFlatColor(SceneMaterial& mat, float r, float g, float b) {
    auto& cv = mat.customShader.constValues;
    if (auto it = cv.find("g_Color"); it != cv.end() && it->second.size() >= 3) {
        it->second[0]                     = r;
        it->second[1]                     = g;
        it->second[2]                     = b;
        mat.customShader.constValuesDirty = true;
    }
}

} // namespace wallpaper
