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

// Vec3 property keyframe animation:
//   "<prop>": { "animation": { "c0":[…], "c1":[…], "c2":[…],
//                              "options":{name,fps,length,mode,startpaused},
//                              "relative":<bool> },
//               "value": "x y z" }
// WE stores one curve per axis (c0=x, c1=y, c2=z) on the same animation.  We
// emit three PropertyAnimation entries — one per axis — with property names
// "<prop>.x" / "<prop>.y" / "<prop>.z" so tickPropertyAnimations can dispatch
// per component.  Each entry's initialValue carries the corresponding axis of
// the JSON `value` string so relative mode can compose deltas onto the base.
//
// Drove the Rella whale (3363252053 id=173) fish school stuck at static origin
// because origin.animation was silently dropped — alpha had its own parser
// path but origin/scale/angles fell through.
bool TryParseVec3PropertyAnimation(const nlohmann::json&           fieldJson,
                                   std::string_view                propertyName,
                                   const std::array<float, 3>&     fallbackBase,
                                   std::vector<PropertyAnimation>& out) {
    if (! fieldJson.is_object()) return false;
    if (! fieldJson.contains("animation")) return false;
    const auto& animJson = fieldJson.at("animation");
    if (! animJson.is_object()) return false;

    // Parse shared option fields once.
    std::string      animName;
    float            fps    = 30.0f;
    float            length = 0.0f;
    PropertyAnimMode mode   = PropertyAnimMode::Loop;
    bool             startPaused = false;
    bool             wraploop    = false;
    if (animJson.contains("options") && animJson.at("options").is_object()) {
        const auto& opts = animJson.at("options");
        GET_JSON_NAME_VALUE_NOWARN(opts, "name", animName);
        GET_JSON_NAME_VALUE_NOWARN(opts, "fps", fps);
        GET_JSON_NAME_VALUE_NOWARN(opts, "length", length);
        std::string modeStr;
        GET_JSON_NAME_VALUE_NOWARN(opts, "mode", modeStr);
        mode = ParsePropertyAnimMode(modeStr);
        GET_JSON_NAME_VALUE_NOWARN(opts, "startpaused", startPaused);
        GET_JSON_NAME_VALUE_NOWARN(opts, "wraploop", wraploop);
    }
    bool relative = false;
    GET_JSON_NAME_VALUE_NOWARN(animJson, "relative", relative);

    constexpr std::array<const char*, 3> kChannelKeys { "c0", "c1", "c2" };
    constexpr std::array<const char*, 3> kAxisSuffix { ".x", ".y", ".z" };

    bool emittedAny = false;
    for (int axis = 0; axis < 3; ++axis) {
        if (! animJson.contains(kChannelKeys[axis])) continue;
        const auto& channelJson = animJson.at(kChannelKeys[axis]);
        if (! channelJson.is_array() || channelJson.empty()) continue;

        PropertyAnimation panim {};
        panim.name        = animName.empty() ? std::string(propertyName) : animName;
        panim.property    = std::string(propertyName) + kAxisSuffix[axis];
        panim.fps         = fps;
        panim.length      = length;
        panim.mode        = mode;
        panim.startPaused = startPaused;
        panim.relative    = relative;
        panim.wraploop    = wraploop;
        panim.initialValue = fallbackBase[(std::size_t)axis];

        for (const auto& kfJson : channelJson) {
            PropertyAnimKeyframe kf {};
            GET_JSON_NAME_VALUE_NOWARN(kfJson, "frame", kf.frame);
            GET_JSON_NAME_VALUE_NOWARN(kfJson, "value", kf.value);
            panim.keyframes.push_back(kf);
        }

        LOG_INFO("vec3 prop anim parsed: prop=%s name='%s' mode=%d fps=%.2f len=%.2f "
                 "paused=%d relative=%d wraploop=%d keys=%zu base=%.3f",
                 panim.property.c_str(),
                 panim.name.c_str(),
                 (int)panim.mode,
                 panim.fps,
                 panim.length,
                 (int)panim.startPaused,
                 (int)panim.relative,
                 (int)panim.wraploop,
                 panim.keyframes.size(),
                 panim.initialValue);
        out.push_back(std::move(panim));
        emittedAny = true;
    }
    return emittedAny;
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
    // (empty) Mechanism retained for any effect that genuinely must be
    // suppressed.  "2799421411" (Audio Responsive Oscilloscope) was previously
    // listed for a Vulkan deadlock; re-enabled after the renderer's sync/
    // render-graph fixes — verified to render without deadlock (2992803622).
    // NOTE: blacklisting forces visible=false, which the effect loader skips
    // and *compacts* the loaded index, while shader-value scripts key on the
    // JSON effect index — so a blacklisted effect silently breaks the alpha/
    // uniform scripts of every effect after it on the same layer.
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
        // Shape-quad effects with DIRECTDRAW=1 (e.g. effects/lightshafts in
        // 3287715210) have shaders that already premultiply the output RGB
        // by the mask/alpha and the inner ApplyBlending often resolves to
        // additive math (BLENDMODE=31 → `A + B*opacity`).  The straight-
        // alpha Translucent blend then re-multiplies by alpha producing
        // `fx² * fxColor` AND darkens the background by `(1-fx)` — rays
        // appear as dim/dark stripes instead of the authored bright beams.
        // Switch the synthetic material to additive so the final composite
        // uses `src + dst` and preserves both the authored ray brightness
        // and the background.  The lightshafts/lensflare/motionblur effect
        // family all share this DIRECTDRAW + procedural shape-quad pattern.
        for (const auto& eff : effects) {
            bool eff_uses_directdraw = false;
            for (const auto& pass : eff.passes) {
                auto it = pass.combos.find("DIRECTDRAW");
                if (it != pass.combos.end() && it->second != 0) {
                    eff_uses_directdraw = true;
                    break;
                }
            }
            if (eff_uses_directdraw) {
                material.blending = "additive";
                break;
            }
        }
        if (json.contains("config")) {
            const auto& jConf = json.at("config");
            GET_JSON_NAME_VALUE_NOWARN(jConf, "passthrough", config.passthrough);
        }
        if (json.contains("dependencies") && json.at("dependencies").is_array()) {
            for (const auto& jD : json.at("dependencies")) {
                if (jD.is_number_integer()) dependencies.push_back(jD.get<int32_t>());
            }
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
    // Font-extension files (.ttf / .otf) referenced via the `image` slot
    // are an author-side schema bug — fonts should be referenced as text-
    // layer attributes, not image-object materials.  Driver: Uncle Panda
    // (3544533690) ships 20+ such mis-references.  Skip silently before
    // PARSE_JSON so its own ERROR log doesn't fire either; the wallpaper
    // stays renderable for everything that ISN'T a stray font object.
    {
        auto is_font_path = [](const std::string& p) {
            for (const char* ext : { ".ttf", ".otf", ".TTF", ".OTF" })
                if (p.size() >= 4 &&
                    p.compare(p.size() - 4, 4, ext) == 0)
                    return true;
            return false;
        };
        if (is_font_path(image)) {
            LOG_INFO("skipping font-as-image reference: %s (author schema bug)",
                     image.c_str());
            return false;
        }
    }
    nlohmann::json jImage;
    if (! PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + image), jImage)) {
        LOG_ERROR("Can't load image json: %s", image.c_str());
        return false;
    }
    GET_JSON_NAME_VALUE_NOWARN(jImage, "fullscreen", fullscreen);
    GET_JSON_NAME_VALUE_NOWARN(jImage, "autosize", autosize);
    GET_JSON_NAME_VALUE_NOWARN(jImage, "solidlayer", solidlayer);
    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent_id);
    GET_JSON_NAME_VALUE_NOWARN(json, "colorBlendMode", colorBlendMode);
    GET_JSON_NAME_VALUE_NOWARN(json, "copybackground", copybackground);
    if (! fullscreen) {
        GET_JSON_NAME_VALUE(json, "origin", origin);
        GET_JSON_NAME_VALUE(json, "angles", angles);
        GET_JSON_NAME_VALUE(json, "scale", scale);
        GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
        // GET_JSON_NAME_VALUE already extracts the `value` field from object-wrapped
        // origin/scale/angles (so the static base is correct), but the embedded
        // c0/c1/c2 keyframe tracks fell on the floor.  Pull them into the
        // propertyAnimations vector now so tickPropertyAnimations can drive
        // the node transform each frame.
        if (json.contains("origin") && json.at("origin").is_object()) {
            TryParseVec3PropertyAnimation(json.at("origin"), "origin", origin,
                                          propertyAnimations);
        }
        if (json.contains("scale") && json.at("scale").is_object()) {
            TryParseVec3PropertyAnimation(json.at("scale"), "scale", scale,
                                          propertyAnimations);
        }
        if (json.contains("angles") && json.at("angles").is_object()) {
            TryParseVec3PropertyAnimation(json.at("angles"), "angles", angles,
                                          propertyAnimations);
        }
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
    if (json.contains("dependencies") && json.at("dependencies").is_array()) {
        for (const auto& jD : json.at("dependencies")) {
            if (jD.is_number_integer()) dependencies.push_back(jD.get<int32_t>());
        }
    }
    return true;
}
