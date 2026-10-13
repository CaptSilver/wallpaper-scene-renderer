#include "WPMaterial.h"
#include "WPAnimationJson.h"
#include "UserPropFloatVector.hpp"
#include "UserPropUseRecorder.hpp"
#include "WPUserProperties.hpp"
#include <functional>
#include <optional>
#include <unordered_map>

using namespace wallpaper::wpscene;

namespace
{

// The textures/usertextures/constantshadervalues/combos parse is identical
// between WPMaterialPass::FromJson and WPMaterial::FromJson's own passes[0]
// block -- this shares it.  `onConstantValue`, when given, runs once per
// constantshadervalues entry with its name and raw (pre-resolution) JSON, and
// returns whether it recorded a runtime binding for that constant.  A bound
// constant is updated live, so its {"user": ...} read is not reported as a
// parse-time use; an unbound one is (the resolver reports it), so a change to
// the property reloads the scene instead of being lost.  Callers that care
// about {"user": ...} references go through ParsePassFieldsRecordingBindings
// instead.
void ParsePassFields(
    const nlohmann::json& json, std::vector<std::string>& textures,
    std::vector<WPMaterialPassUserTexture>&                               usertextures,
    std::unordered_map<std::string, int32_t>&                             combos,
    std::unordered_map<std::string, std::vector<float>>&                  constantshadervalues,
    std::unordered_map<std::string, wallpaper::ConstantAnimation>&        constantAnimations,
    const std::function<bool(const std::string&, const nlohmann::json&)>& onConstantValue = {}) {
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
            bool               read = false;
            GET_JSON_VALUE(jC.key(), name);
            const bool bound = onConstantValue && onConstantValue(name, jC.value());
            {
                std::optional<wallpaper::LiveBoundReadScope> live;
                if (bound) live.emplace();
                read = GET_JSON_VALUE(jC.value(), value);
            }
            // A failed read leaves nothing to store; an empty entry would
            // overwrite the seeded shader default with no value at all.
            if (! read) continue;
            constantshadervalues[name] = value;
            if (auto anim = ParseConstantAnimation(jC.value(), value))
                constantAnimations[name] = std::move(*anim);
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
void MergeOverlayFields(
    std::vector<std::string>& textures, std::vector<WPMaterialPassUserTexture>& usertextures,
    std::unordered_map<std::string, int32_t>&                      combos,
    std::unordered_map<std::string, std::vector<float>>&           constantshadervalues,
    std::unordered_map<std::string, wallpaper::ConstantAnimation>& constantAnimations,
    std::unordered_map<std::string, std::string>& userShaderBindings, const WPMaterialPass& p) {
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
        // A plain value replaces whatever curve the base carried for it.
        if (p.constantAnimations.count(el.first) == 0) constantAnimations.erase(el.first);
        // Same for a property link: the override's value wins, so the base's property
        // must stop driving this constant (the override's own link, if any, is added below).
        userShaderBindings.erase(el.first);
    }
    for (const auto& el : p.userShaderBindings) {
        userShaderBindings[el.first] = el.second;
    }
    for (const auto& el : p.constantAnimations) {
        constantAnimations[el.first] = el.second;
    }
    for (const auto& el : p.combos) {
        combos[el.first] = el.second;
    }
}

// GET_JSON_VALUE already resolves an inline {"user": ...} to the property's
// current value, but the link back to the property is lost with it. Record it,
// keyed by constant (one constant has one property; a property can drive
// several constants), so a settings change can reach the uniform without
// reparsing the scene. Shared by the material file's pass and the scene's
// per-effect pass override. The live update writes the property's raw value and
// cannot evaluate a {"user": {"name", "condition"}} condition, so the pass
// override leaves that shape unbound (a reload resolves it correctly). A constant
// whose default is a bool is unbound in both, see the check below.
void ParsePassFieldsRecordingBindings(
    const nlohmann::json& json, std::vector<std::string>& textures,
    std::vector<WPMaterialPassUserTexture>&                        usertextures,
    std::unordered_map<std::string, int32_t>&                      combos,
    std::unordered_map<std::string, std::vector<float>>&           constantshadervalues,
    std::unordered_map<std::string, wallpaper::ConstantAnimation>& constantAnimations,
    std::unordered_map<std::string, std::string>& userShaderBindings, bool bindConditional) {
    ParsePassFields(json,
                    textures,
                    usertextures,
                    combos,
                    constantshadervalues,
                    constantAnimations,
                    [&userShaderBindings, bindConditional](const std::string&    name,
                                                           const nlohmann::json& rawValue) {
                        auto prop = wallpaper::UserPropertyNameOf(rawValue);
                        if (prop.empty()) return false;
                        // UserPropertyNameOf found a "user" key, so .at() cannot throw.
                        if (! bindConditional && ! rawValue.at("user").is_string()) return false;
                        // A bool default makes parse time resolve to a match flag or a
                        // default-flip, while the live write carries the property's own
                        // value, so the two cannot agree; leave it to a reload.
                        if (rawValue.contains("value") && rawValue.at("value").is_boolean())
                            return false;
                        userShaderBindings[name] = prop;
                        return true;
                    });
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
    MergeOverlayFields(textures,
                       usertextures,
                       combos,
                       constantshadervalues,
                       constantAnimations,
                       userShaderBindings,
                       p);
}

void WPMaterial::MergePass(const WPMaterialPass& p) {
    MergeOverlayFields(textures,
                       usertextures,
                       combos,
                       constantshadervalues,
                       constantAnimations,
                       userShaderBindings,
                       p);
}

bool WPMaterialPass::FromJson(const nlohmann::json& json) {
    ParsePassFieldsRecordingBindings(json,
                                     textures,
                                     usertextures,
                                     combos,
                                     constantshadervalues,
                                     constantAnimations,
                                     userShaderBindings,
                                     false);
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
    ParsePassFieldsRecordingBindings(jContent,
                                     textures,
                                     usertextures,
                                     combos,
                                     constantshadervalues,
                                     constantAnimations,
                                     userShaderBindings,
                                     true);
    // Resolve usershadervalues: maps user property names to shader constant names
    if (jContent.contains("usershadervalues")) {
        for (const auto& entry : jContent.at("usershadervalues").items()) {
            std::string shaderConstName = entry.value().get<std::string>();
            // Record the binding for runtime updates
            userShaderBindings[shaderConstName] = entry.key();
            if (g_currentUserProperties != nullptr) {
                auto propVal = g_currentUserProperties->GetProperty(entry.key());
                if (! propVal.has_value()) continue;
                const auto&        val      = *propVal;
                std::vector<float> floatVec = wek::ParseFloatVectorFromJson(val);
                if (! floatVec.empty()) {
                    constantshadervalues[shaderConstName] = floatVec;
                }
            }
        }
    }
    return true;
}