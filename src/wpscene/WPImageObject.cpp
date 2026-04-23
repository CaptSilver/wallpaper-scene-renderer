#include "WPImageObject.h"
#include "Utils/Logging.h"
#include "Fs/VFS.h"

#include <sstream>

using namespace wallpaper::wpscene;

namespace
{
using wallpaper::ParsePropertyAnimMode;
using wallpaper::PropertyAnimation;
using wallpaper::PropertyAnimKeyframe;
using wallpaper::PropertyAnimMode;

// Try to extract a scalar property's embedded keyframe animation:
//   "<prop>": { "animation": { "c0":[…], "options":{name,fps,length,mode,startpaused} },
//               "value": <fallback> }
// Returns true iff an animation was found and parsed.  Silently returns
// false for the common case where <prop> is a plain scalar.
bool TryParsePropertyAnimation(const nlohmann::json& fieldJson, std::string_view propertyName,
                               float fallbackInitial, PropertyAnimation& out) {
    if (! fieldJson.is_object()) return false;
    if (! fieldJson.contains("animation")) return false;
    const auto& animJson = fieldJson.at("animation");
    if (! animJson.is_object()) return false;

    out              = PropertyAnimation {};
    out.property     = std::string(propertyName);
    out.initialValue = fallbackInitial;
    if (fieldJson.contains("value") && fieldJson.at("value").is_number()) {
        out.initialValue = fieldJson.at("value").get<float>();
    }

    if (animJson.contains("options") && animJson.at("options").is_object()) {
        const auto& opts = animJson.at("options");
        GET_JSON_NAME_VALUE_NOWARN(opts, "name", out.name);
        float fps    = out.fps;
        float length = out.length;
        GET_JSON_NAME_VALUE_NOWARN(opts, "fps", fps);
        GET_JSON_NAME_VALUE_NOWARN(opts, "length", length);
        out.fps    = fps;
        out.length = length;
        std::string mode;
        GET_JSON_NAME_VALUE_NOWARN(opts, "mode", mode);
        out.mode = ParsePropertyAnimMode(mode);
        bool sp  = false;
        GET_JSON_NAME_VALUE_NOWARN(opts, "startpaused", sp);
        out.startPaused = sp;
    }

    // WE stores keyframes under "c0" for single-channel tracks, "c0"/"c1"/"c2"
    // for multi-channel.  We only handle scalar (c0) today; color vec3 support
    // would extend this.
    if (animJson.contains("c0") && animJson.at("c0").is_array()) {
        for (const auto& kfJson : animJson.at("c0")) {
            PropertyAnimKeyframe kf {};
            GET_JSON_NAME_VALUE_NOWARN(kfJson, "frame", kf.frame);
            GET_JSON_NAME_VALUE_NOWARN(kfJson, "value", kf.value);
            out.keyframes.push_back(kf);
        }
    }

    if (out.name.empty()) {
        // WE allows unnamed animations; scripts can't address them, but they
        // should still play if not startPaused.  Give a synthetic name derived
        // from the property for script lookup determinism.
        out.name = std::string(propertyName);
    }

    LOG_INFO("property anim parsed: prop=%s name='%s' mode=%d fps=%.2f len=%.2f paused=%d keys=%zu",
             out.property.c_str(),
             out.name.c_str(),
             (int)out.mode,
             out.fps,
             out.length,
             (int)out.startPaused,
             out.keyframes.size());
    return true;
}

// Parse color field: may be a simple vec3 array or an object with {script, scriptproperties, value}
void ParseColorField(const nlohmann::json& json, std::array<float, 3>& color,
                     std::string& colorScript, std::string& colorScriptProperties, int32_t id) {
    if (json.contains("color") && json.at("color").is_object() &&
        json.at("color").contains("script")) {
        const auto& colorObj = json.at("color");
        if (colorObj.contains("script") && colorObj.at("script").is_string())
            colorScript = colorObj.at("script").get<std::string>();
        if (colorObj.contains("scriptproperties"))
            colorScriptProperties = colorObj.at("scriptproperties").dump();
        if (colorObj.contains("value") && colorObj.at("value").is_string()) {
            std::istringstream iss(colorObj.at("value").get<std::string>());
            iss >> color[0] >> color[1] >> color[2];
        }
        LOG_INFO("color script on id=%d (len=%zu)", id, colorScript.size());
    } else {
        GET_JSON_NAME_VALUE_NOWARN(json, "color", color);
    }
}
} // namespace

bool WPEffectCommand::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "command", command);
    GET_JSON_NAME_VALUE(json, "target", target);
    GET_JSON_NAME_VALUE(json, "source", source);
    return true;
}

bool WPEffectFbo::FromJson(const nlohmann::json& json) {
    GET_JSON_NAME_VALUE(json, "name", name);
    GET_JSON_NAME_VALUE(json, "format", format);

    GET_JSON_NAME_VALUE(json, "scale", scale);
    if (scale == 0) {
        LOG_ERROR("fbo scale can't be 0");
        scale = 1;
    }
    return true;
}

// Define and initialize the static property
const std::unordered_set<std::string> WPImageEffect::BLACKLISTED_WORKSHOP_EFFECTS = {
    "2799421411" // Audio Responsive Oscilloscope   --  causes vulcan deadlock
};

bool WPImageEffect::IsEffectBlacklisted(const std::string& filePath) {
    std::filesystem::path path(filePath);
    // Check if the path has a parent path
    if (path.has_parent_path()) {
        path = path.parent_path();
        if (path.has_parent_path()) {
            std::string effectId   = path.parent_path().filename().string();
            std::string parentPath = path.parent_path().string();
            return WPImageEffect::BLACKLISTED_WORKSHOP_EFFECTS.find(effectId) !=
                   WPImageEffect::BLACKLISTED_WORKSHOP_EFFECTS.end();
        }
    }
    return false;
}

bool WPImageEffect::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    std::string filePath;
    GET_JSON_NAME_VALUE(json, "file", filePath);
    GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
    if (this->IsEffectBlacklisted(filePath)) {
        // hide blacklisted effects
        visible = false;
    }
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    nlohmann::json jEffect;
    if (! PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + filePath), jEffect)) return false;
    if (! FromFileJson(jEffect, vfs)) return false;

    if (json.contains("passes")) {
        const auto& jPasses = json.at("passes");
        if (jPasses.size() > passes.size()) {
            LOG_ERROR("passes is not injective");
            return false;
        }
        int32_t i = 0;
        for (const auto& jP : jPasses) {
            WPMaterialPass pass;
            pass.FromJson(jP);
            passes[i++].Update(pass);
        }
    }
    return true;
}

bool WPImageEffect::FromFileJson(const nlohmann::json& json, fs::VFS& vfs) {
    GET_JSON_NAME_VALUE_NOWARN(json, "version", version);
    GET_JSON_NAME_VALUE(json, "name", name);
    if (json.contains("fbos")) {
        for (auto& jF : json.at("fbos")) {
            WPEffectFbo fbo;
            fbo.FromJson(jF);
            fbos.push_back(std::move(fbo));
        }
    }
    if (json.contains("passes")) {
        const auto& jEPasses = json.at("passes");
        bool        compose { false };
        for (const auto& jP : jEPasses) {
            if (! jP.contains("material")) {
                if (jP.contains("command")) {
                    WPEffectCommand cmd;
                    cmd.FromJson(jP);
                    cmd.afterpos = passes.size();
                    commands.push_back(cmd);
                    continue;
                }
                LOG_ERROR("no material in effect pass");
                return false;
            }
            std::string matPath;
            GET_JSON_NAME_VALUE(jP, "material", matPath);
            nlohmann::json jMat;
            if (! PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + matPath), jMat)) return false;
            WPMaterial material;
            material.FromJson(jMat);
            materials.push_back(std::move(material));
            WPMaterialPass pass;
            pass.FromJson(jP);
            passes.push_back(std::move(pass));
            if (jP.contains("compose")) GET_JSON_NAME_VALUE(jP, "compose", compose);
        }
        if (compose) {
            if (passes.size() != 2) {
                LOG_ERROR("effect compose option error");
                return false;
            }
            WPEffectFbo fbo;
            {
                fbo.name  = "_rt_FullCompoBuffer1";
                fbo.scale = 1;
            }
            fbos.push_back(fbo);
            passes.at(0).bind.push_back({ "previous", 0 });
            passes.at(0).target = "_rt_FullCompoBuffer1";
            passes.at(1).bind.push_back({ "_rt_FullCompoBuffer1", 0 });
        }
    } else {
        LOG_ERROR("no passes in effect file");
        return false;
    }
    return true;
}

bool WPImageObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    // Shape-quad objects: procedural effect quads with no image file
    if (json.contains("shape") && ! json.contains("image")) {
        std::string shape;
        GET_JSON_NAME_VALUE(json, "shape", shape);
        if (shape != "quad") {
            LOG_ERROR("unsupported shape type: %s", shape.c_str());
            return false;
        }

        GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
        GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
        GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
        GET_JSON_NAME_VALUE_NOWARN(json, "disablepropagation", disablepropagation);
        // Combo condition-based visibility: {"user": {"condition": "X", "name": "prop"}}
        // These are mutually exclusive character selectors that must stay in the
        // main render graph (not offscreen) so they can be toggled at runtime.
        if (json.contains("visible") && json.at("visible").is_object() &&
            json.at("visible").contains("user") && json.at("visible").at("user").is_object() &&
            json.at("visible").at("user").contains("condition"))
            visibleIsComboSelector = true;
        GET_JSON_NAME_VALUE_NOWARN(json, "perspective", perspective);
        GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent_id);
        GET_JSON_NAME_VALUE_NOWARN(json, "origin", origin);
        GET_JSON_NAME_VALUE_NOWARN(json, "scale", scale);
        GET_JSON_NAME_VALUE_NOWARN(json, "angles", angles);
        GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
        ParseColorField(json, color, colorScript, colorScriptProperties, id);
        GET_JSON_NAME_VALUE_NOWARN(json, "alpha", alpha);
        GET_JSON_NAME_VALUE_NOWARN(json, "brightness", brightness);
        GET_JSON_NAME_VALUE_NOWARN(json, "colorBlendMode", colorBlendMode);

        if (json.contains("size")) {
            GET_JSON_NAME_VALUE(json, "size", size);
        }
        // else keep default (2,2) — ParseImageObj will override with ortho size

        // Synthetic material: genericimage2, translucent blend.
        material.shader = "genericimage2";

        LOG_INFO("shape-quad object id=%d name='%s'", id, name.c_str());

        if (json.contains("effects")) {
            for (const auto& jE : json.at("effects")) {
                WPImageEffect wpeff;
                wpeff.FromJson(jE, vfs);
                effects.push_back(std::move(wpeff));
            }
        }
        if (json.contains("config")) {
            const auto& jConf = json.at("config");
            GET_JSON_NAME_VALUE_NOWARN(jConf, "passthrough", config.passthrough);
        }
        return true;
    }

    GET_JSON_NAME_VALUE(json, "image", image);
    GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
    GET_JSON_NAME_VALUE_NOWARN(json, "disablepropagation", disablepropagation);
    if (json.contains("visible") && json.at("visible").is_object() &&
        json.at("visible").contains("user") && json.at("visible").at("user").is_object() &&
        json.at("visible").at("user").contains("condition"))
        visibleIsComboSelector = true;
    GET_JSON_NAME_VALUE_NOWARN(json, "perspective", perspective);
    GET_JSON_NAME_VALUE_NOWARN(json, "alignment", alignment);
    GET_JSON_NAME_VALUE_NOWARN(json, "attachment", attachment);
    nlohmann::json jImage;
    if (! PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + image), jImage)) {
        LOG_ERROR("Can't load image json: %s", image.c_str());
        return false;
    }
    GET_JSON_NAME_VALUE_NOWARN(jImage, "fullscreen", fullscreen);
    GET_JSON_NAME_VALUE_NOWARN(jImage, "autosize", autosize);
    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent_id);
    GET_JSON_NAME_VALUE_NOWARN(json, "colorBlendMode", colorBlendMode);
    if (! fullscreen) {
        GET_JSON_NAME_VALUE(json, "origin", origin);
        GET_JSON_NAME_VALUE(json, "angles", angles);
        GET_JSON_NAME_VALUE(json, "scale", scale);
        GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
        if (jImage.contains("width")) {
            int32_t w, h;
            GET_JSON_NAME_VALUE(jImage, "width", w);
            GET_JSON_NAME_VALUE(jImage, "height", h);
            size     = { (float)w, (float)h };
            autosize = false; // explicit dimensions win
        } else if (json.contains("size")) {
            GET_JSON_NAME_VALUE(json, "size", size);
            autosize = false; // scene-level size wins
        } else if (autosize) {
            // Leave size at default (2,2); ParseImageObj resolves from the
            // first texture's sprite frame dimensions.
        } else {
            size = { origin.at(0) * 2, origin.at(1) * 2 };
        }
    }
    GET_JSON_NAME_VALUE_NOWARN(jImage, "nopadding", nopadding);
    ParseColorField(json, color, colorScript, colorScriptProperties, id);
    GET_JSON_NAME_VALUE_NOWARN(json, "alpha", alpha);
    if (json.contains("alpha") && json.at("alpha").is_object()) {
        PropertyAnimation panim;
        if (TryParsePropertyAnimation(json.at("alpha"), "alpha", alpha, panim)) {
            propertyAnimations.push_back(std::move(panim));
        }
    }
    GET_JSON_NAME_VALUE_NOWARN(json, "brightness", brightness);

    GET_JSON_NAME_VALUE_NOWARN(jImage, "puppet", puppet);
    if (jImage.contains("material")) {
        std::string matPath;
        GET_JSON_NAME_VALUE(jImage, "material", matPath);
        nlohmann::json jMat;
        if (! PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + matPath), jMat)) {
            LOG_ERROR("Can't load material json: %s", matPath.c_str());
            return false;
        }
        material.FromJson(jMat);
    } else {
        LOG_INFO("image object no material");
        return false;
    }
    if (json.contains("effects")) {
        for (const auto& jE : json.at("effects")) {
            WPImageEffect wpeff;
            wpeff.FromJson(jE, vfs);
            effects.push_back(std::move(wpeff));
        }
    }
    if (json.contains("animationlayers")) {
        for (const auto& jLayer : json.at("animationlayers")) {
            WPPuppetLayer::AnimationLayer layer;
            GET_JSON_NAME_VALUE(jLayer, "animation", layer.id);
            GET_JSON_NAME_VALUE(jLayer, "blend", layer.blend);
            GET_JSON_NAME_VALUE(jLayer, "rate", layer.rate);
            GET_JSON_NAME_VALUE_NOWARN(jLayer, "visible", layer.visible);
            puppet_layers.push_back(layer);
        }
    }
    if (json.contains("config")) {
        const auto& jConf = json.at("config");
        GET_JSON_NAME_VALUE_NOWARN(jConf, "passthrough", config.passthrough);
    }
    return true;
}
