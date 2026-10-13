#pragma once

// Shader-uniform bindings fed by user properties: resolving the GLSL name a
// material constant maps to, recording the binding when the parser builds a
// material, and writing a changed property into the bound materials.
//
// Header-only so the parser and the live-update path share one definition and
// both can be exercised without the wallpaper bridge.

#include "Scene/Scene.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"
#include "UserPropRuntimeUpdate.hpp"
#include "Utils/Logging.h"
#include "WPShaderParser.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace wek
{

using wallpaper::Scene;
using wallpaper::SceneMaterial;
using wallpaper::SceneMesh;
using wallpaper::WPShaderInfo;

// The GLSL uniform a material constant name binds to: the shader's alias for
// it, else a uniform whose name minus its `g_` prefix matches.  Empty when the
// shader declares neither.
inline std::string ConstantUniformName(const WPShaderInfo& info, const std::string& name) {
    if (info.alias.count(name) != 0) return info.alias.at(name);
    for (const auto& el : info.alias) {
        // An alias no longer than the `g_` prefix has no name after it to
        // compare (and comparing from past its end would throw).
        if (el.second.size() > 2 && el.second.compare(2, std::string::npos, name) == 0)
            return el.second;
    }
    return {};
}

// ConstantUniformName, falling back to the constant's own name: a user binding
// is still recorded when the shader declares no matching uniform.
inline std::string UserBindingUniformName(const WPShaderInfo& info, const std::string& name) {
    auto glname = ConstantUniformName(info, name);
    return glname.empty() ? name : glname;
}

inline void registerUserPropUniformBinding(Scene& scene, const std::string& propName,
                                           const std::shared_ptr<SceneMesh>& mesh,
                                           const std::string&                glname) {
    scene.userPropUniformBindings[propName].push_back({ mesh, glname });
}

// Writes each property in `props` that has a uniform binding into the bound
// materials and flags them dirty so the next frame uploads the value.
inline void applyUserPropUniforms(const Scene& scene, const nlohmann::json& props) {
    for (const auto& [propName, bindings] : scene.userPropUniformBindings) {
        if (! props.contains(propName)) continue;

        std::vector<float> floatVec = ParseFloatVectorFromJson(props.at(propName));
        if (floatVec.empty()) continue;

        for (const auto& b : bindings) {
            SceneMaterial* material = b.mesh ? b.mesh->Material() : nullptr;
            if (material == nullptr) continue;
            material->customShader.constValues[b.uniformName] = floatVec;
            material->customShader.constValuesDirty           = true;
            LOG_INFO("Runtime user prop: '%s' -> uniform '%s' = [%f...]",
                     propName.c_str(),
                     b.uniformName.c_str(),
                     floatVec[0]);
        }
    }
}

} // namespace wek
