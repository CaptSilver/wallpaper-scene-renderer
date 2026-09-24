#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_set>

#include "SceneShader.h"
#include "Type.hpp"

namespace wallpaper
{

struct SceneMaterialCustomShader {
    std::shared_ptr<SceneShader> shader;
    ShaderValues                 constValues;
    bool                         constValuesDirty { false };
    // Author-facing → GLSL uniform name map, mirrored from the shader's
    // WPShaderInfo at parse time.  Lets the runtime drain
    // (m_pending_(effect_)material_values) resolve a SceneScript write
    // like `material.color = Vec3(...)` to the actual uniform on the
    // bound shader (e.g. tint.frag's g_TintColor — not g_Color).  Without
    // this, Game of Life (3453251764) color-palette buttons all rendered
    // as their authored-default red because the override landed in an
    // unread constValues slot.
    Map<std::string, std::string> alias;
};

// Resolve an author-facing constant name -- what a WE material's JSON or a
// SceneScript write calls it, e.g. "bloomstrength" -- through this shader's
// alias map to the GLSL uniform it actually declared (e.g. "g_BloomStrength").
// A name with no entry passes through unchanged: CustomShaderPass only
// re-uploads a constValues key that matches something in the compiled
// uniform block, so an unresolved name is a silent no-op there, not a crash
// here.  Shared by the runtime SceneScript-write drains in SceneWallpaper.cpp
// (m_pending_material_values / m_pending_effect_material_values already did
// this lookup inline; this gives the pattern a name instead of a third copy).
inline std::string ResolveShaderConstantName(const SceneMaterialCustomShader& shader,
                                             const std::string&               name) {
    auto it = shader.alias.find(name);
    return it != shader.alias.end() ? it->second : name;
}

struct SceneMaterial {
public:
    SceneMaterial()                                = default;
    SceneMaterial(const SceneMaterial&)            = default;
    SceneMaterial(SceneMaterial&&)                 = default;
    SceneMaterial& operator=(const SceneMaterial&) = default;
    SceneMaterial& operator=(SceneMaterial&&)      = default;

    std::string              name;
    std::vector<std::string> textures;
    std::vector<std::string> defines;

    bool hasSprite { false };

    SceneMaterialCustomShader customShader;
    BlendMode                 blenmode { BlendMode::Disable };
    bool                      depthTest { false };
    bool                      depthWrite { false };
    float                     depthBiasConstant { 0 };
    std::string               cullmode { "nocull" };
};
} // namespace wallpaper
