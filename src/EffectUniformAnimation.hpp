#pragma once
#include "Scene/Scene.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"

#include <vector>

namespace wallpaper
{

// Advances every keyframed effect constant by `dt` seconds and writes the ones
// that moved into their material, flagged dirty so the next frame uploads them.
inline void TickEffectUniformAnimations(std::vector<Scene::EffectUniformAnimation>& anims,
                                        double                                      dt) {
    for (auto& a : anims) {
        SceneMaterial* material = a.mesh ? a.mesh->Material() : nullptr;
        if (material == nullptr || ! TickConstantAnimation(a.anim, dt)) continue;
        material->customShader.constValues[a.uniform] = a.anim.value;
        material->customShader.constValuesDirty       = true;
    }
}

} // namespace wallpaper
