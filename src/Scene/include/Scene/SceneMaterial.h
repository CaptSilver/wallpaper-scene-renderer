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

struct SceneMaterial {
public:
    SceneMaterial()                     = default;
    SceneMaterial(const SceneMaterial&) = default;
    SceneMaterial(SceneMaterial&& o)
        : name(std::move(o.name)),
          textures(std::move(o.textures)),
          defines(std::move(o.defines)) {};

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
