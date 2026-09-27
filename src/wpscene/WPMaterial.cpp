#include "WPMaterial.h"
#include "WPUserProperties.hpp"
#include <functional>
#include <sstream>
#include <unordered_map>

using namespace wallpaper::wpscene;

namespace
{

// The user-property reference a constant value carries, if any.  Two shapes
// occur in shipped materials: {"user": "prop"} and the conditional
// {"user": {"name": "prop", "condition": "..."}}.  Returns "" for a plain
// literal.  WPUserProperties::ResolveValue reads the same two shapes to get
// the value out.
std::string UserPropertyNameOf(const nlohmann::json& value) {
    if (! value.is_object() || ! value.contains("user")) return {};
    const auto& user = value.at("user");
    if (user.is_string()) return user.get<std::string>();
    if (user.is_object() && user.contains("name") && user.at("name").is_string())
        return user.at("name").get<std::string>();
    return {};
}

// The textures/usertextures/constantshadervalues/combos parse is identical
// between WPMaterialPass::FromJson and WPMaterial::FromJson's own passes[0]
// block -- this shares it.  `onConstantValue`, when given, runs once per
// constantshadervalues entry with its name and raw (pre-resolution) JSON --
// WPMaterial::FromJson uses it to record userShaderBindings; WPMaterialPass
// has no equivalent use, so it's left unset there.
void ParsePassFields(
    const nlohmann::json& json, std::vector<std::string>& textures,
    std::vector<WPMaterialPassUserTexture>&                               usertextures,
    std::unordered_map<std::string, int32_t>&                             combos,
    std::unordered_map<std::string, std::vector<float>>&                  constantshadervalues,
    const std::function<void(const std::string&, const nlohmann::json&)>& onConstantValue = {}) {
    if (json.contains("textures")) {
        for (const auto& jT : json.at("textures")) {
            std::string tex;
            if (! jT.is_null()) GET_JSON_VALUE(jT, tex);
            textures.push_back(tex);
        }
    }
    if (json.contains("usertextures")) {
        for (const auto& jU : json.at("usertextures")) {
            WPMaterialPassUserTexture ut;
            if (! jU.is_null()) ut.FromJson(jU);
            usertextures.push_back(ut);
        }
    }
    if (json.contains("constantshadervalues")) {
        for (const auto& jC : json.at("constantshadervalues").items()) {
            std::string        name;
            std::vector<float> value;
            GET_JSON_VALUE(jC.key(), name);
            GET_JSON_VALUE(jC.value(), value);
            constantshadervalues[name] = value;
            if (onConstantValue) onConstantValue(name, jC.value());
        }
    }
    if (json.contains("combos")) {
        for (const auto& jC : json.at("combos").items()) {
            std::string name;
            int32_t     value;
            GET_JSON_VALUE(jC.key(), name);
            GET_JSON_VALUE(jC.value(), value);
            combos[name] = value;
        }
    }
}

// The pass-overlay rule shared by WPMaterialPass::Update and WPMaterial::MergePass: layer a
// per-instance pass override onto a base pass or material. An empty texture string (or, for
// usertextures, an empty name) means "no override for this slot", not "clear it", so it's
// skipped rather than written; combos and constantshadervalues are plain last-write-wins maps.
void MergeOverlayFields(std::vector<std::string>&                            textures,
                        std::vector<WPMaterialPassUserTexture>&              usertextures,
                        std::unordered_map<std::string, int32_t>&            combos,
                        std::unordered_map<std::string, std::vector<float>>& constantshadervalues,
                        const WPMaterialPass&                                p) {
    int32_t i = -1;
    for (const auto& el : p.textures) {
        i++;
        if (p.textures.size() > textures.size()) textures.resize(p.textures.size());
        if (! el.empty()) {
            textures[i] = el;
        }
    }
    int32_t j = -1;
    for (const auto& el : p.usertextures) {
        j++;
        if (p.usertextures.size() > usertextures.size()) usertextures.resize(p.usertextures.size());
        if (! el.name.empty()) {
            usertextures[j] = el;
        }
    }
    for (const auto& el : p.constantshadervalues) {
        constantshadervalues[el.first] = el.second;
    }
    for (const auto& el : p.combos) {
        combos[el.first] = el.second;
    }
}

} // namespace

bool WPMaterialPassBindItem::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "name", name);
    GET_JSON_NAME_VALUE(json, "index", index);
    return true;
}

bool WPMaterialPassUserTexture::FromJson(const nlohmann::json& json) {
    // A bare string is a plain reference name (app-launcher custom-icon
    // slots ship this shape) -- there's no "type" to read.
    if (json.is_string()) {
        GET_JSON_VALUE(json, name);
        return true;
    }
    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "type", type);
    return true;
}

void WPMaterialPass::Update(const WPMaterialPass& p) {
    MergeOverlayFields(textures, usertextures, combos, constantshadervalues, p);
}

void WPMaterial::MergePass(const WPMaterialPass& p) {
    MergeOverlayFields(textures, usertextures, combos, constantshadervalues, p);
}

bool WPMaterialPass::FromJson(const nlohmann::json& json) {
    ParsePassFields(json, textures, usertextures, combos, constantshadervalues);
    GET_JSON_NAME_VALUE_NOWARN(json, "target", target);
    if (json.contains("bind")) {
        for (const auto& jB : json.at("bind")) {
            WPMaterialPassBindItem bindItem;
            bindItem.FromJson(jB);
            bind.push_back(bindItem);
        }
    }
    return true;
}

bool WPMaterial::FromJson(const nlohmann::json& json) {
    if (! json.contains("passes") || json.at("passes").size() == 0) {
        LOG_ERROR("material no data");
        return false;
    }
    const auto jContent = json.at("passes").at(0);
    if (! jContent.contains("shader")) {
        LOG_ERROR("material no shader");
        return false;
    }
    GET_JSON_NAME_VALUE(jContent, "blending", blending);
    GET_JSON_NAME_VALUE(jContent, "cullmode", cullmode);
    GET_JSON_NAME_VALUE(jContent, "depthtest", depthtest);
    GET_JSON_NAME_VALUE(jContent, "depthwrite", depthwrite);
    // Some WE materials use alternate spellings
    if (jContent.contains("depthtesting") && ! jContent.contains("depthtest"))
        GET_JSON_NAME_VALUE(jContent, "depthtesting", depthtest);
    if (jContent.contains("depthwriting") && ! jContent.contains("depthwrite"))
        GET_JSON_NAME_VALUE(jContent, "depthwriting", depthwrite);
    GET_JSON_NAME_VALUE(jContent, "shader", shader);
    // GET_JSON_VALUE already resolved an inline {"user": ...} to the
    // property's current value, but the link back to the property is lost
    // with it. Record it so a settings change can reach the uniform without
    // reparsing the scene.
    ParsePassFields(jContent,
                    textures,
                    usertextures,
                    combos,
                    constantshadervalues,
                    [this](const std::string& name, const nlohmann::json& rawValue) {
                        if (auto prop = UserPropertyNameOf(rawValue); ! prop.empty())
                            userShaderBindings[prop] = name;
                    });
    // Resolve usershadervalues: maps user property names to shader constant names
    if (jContent.contains("usershadervalues")) {
        for (const auto& entry : jContent.at("usershadervalues").items()) {
            std::string shaderConstName = entry.value().get<std::string>();
            // Record the binding for runtime updates
            userShaderBindings[entry.key()] = shaderConstName;
            if (g_currentUserProperties != nullptr) {
                auto propVal = g_currentUserProperties->GetProperty(entry.key());
                if (! propVal.has_value()) continue;
                const auto&        val = *propVal;
                std::vector<float> floatVec;
                if (val.is_string()) {
                    std::istringstream iss(val.get<std::string>());
                    float              f;
                    while (iss >> f) floatVec.push_back(f);
                } else if (val.is_number()) {
                    floatVec.push_back(val.get<float>());
                } else if (val.is_array()) {
                    for (const auto& elem : val) {
                        if (elem.is_number()) floatVec.push_back(elem.get<float>());
                    }
                }
                if (! floatVec.empty()) {
                    constantshadervalues[shaderConstName] = floatVec;
                }
            }
        }
    }
    return true;
}