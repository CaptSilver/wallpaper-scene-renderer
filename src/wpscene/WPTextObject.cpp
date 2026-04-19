#include "WPTextObject.h"
#include "Utils/Logging.h"
#include "Fs/VFS.h"

using namespace wallpaper::wpscene;

bool WPTextObject::FromJson(const nlohmann::json& json, fs::VFS& vfs) {
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent_id);
    GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
    GET_JSON_NAME_VALUE_NOWARN(json, "origin", origin);
    GET_JSON_NAME_VALUE_NOWARN(json, "scale", scale);
    GET_JSON_NAME_VALUE_NOWARN(json, "angles", angles);
    GET_JSON_NAME_VALUE_NOWARN(json, "size", size);
    GET_JSON_NAME_VALUE_NOWARN(json, "color", color);
    GET_JSON_NAME_VALUE_NOWARN(json, "alpha", alpha);
    GET_JSON_NAME_VALUE_NOWARN(json, "brightness", brightness);
    GET_JSON_NAME_VALUE_NOWARN(json, "colorBlendMode", colorBlendMode);
    GET_JSON_NAME_VALUE_NOWARN(json, "font", font);
    GET_JSON_NAME_VALUE_NOWARN(json, "pointsize", pointsize);
    // Record user property binding for runtime pointsize updates
    if (json.contains("pointsize") && json.at("pointsize").is_object() &&
        json.at("pointsize").contains("user")) {
        const auto& userField = json.at("pointsize")["user"];
        if (userField.is_string()) pointsizeUserProp = userField.get<std::string>();
    }
    GET_JSON_NAME_VALUE_NOWARN(json, "horizontalalign", horizontalalign);
    GET_JSON_NAME_VALUE_NOWARN(json, "verticalalign", verticalalign);
    GET_JSON_NAME_VALUE_NOWARN(json, "anchor", anchor);
    GET_JSON_NAME_VALUE_NOWARN(json, "padding", padding);
    GET_JSON_NAME_VALUE_NOWARN(json, "maxwidth", maxwidth);
    GET_JSON_NAME_VALUE_NOWARN(json, "maxrows", maxrows);

    // Extract text.value and text.script from the "text" sub-object
    if (json.contains("text") && ! json.at("text").is_null()) {
        const auto& jText = json.at("text");
        if (jText.is_object()) {
            GET_JSON_NAME_VALUE_NOWARN(jText, "value", textValue);
            GET_JSON_NAME_VALUE_NOWARN(jText, "script", textScript);
            if (jText.contains("scriptproperties"))
                textScriptProperties = jText.at("scriptproperties").dump();
        } else if (jText.is_string()) {
            textValue = jText.get<std::string>();
        }
    }

    if (json.contains("effects")) {
        for (const auto& jE : json.at("effects")) {
            WPImageEffect wpeff;
            wpeff.FromJson(jE, vfs);
            effects.push_back(std::move(wpeff));
        }
    }

    LOG_INFO("WPTextObject: id=%d name='%s' font='%s' pointsize=%.1f text='%s'",
             id,
             name.c_str(),
             font.c_str(),
             pointsize,
             textValue.c_str());
    return true;
}
