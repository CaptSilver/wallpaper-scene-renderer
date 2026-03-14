#pragma once
#include <cstdint>
#include <unordered_map>
#include <cstdint>
#include "WPJson.hpp"
#include <nlohmann/json.hpp>

namespace wallpaper
{
namespace fs
{
class VFS;
}

namespace wpscene
{

struct WPSoundObject {
    std::string              playbackmode { "loop" };
    float                    maxtime { 10.0f };
    float                    mintime { 0.0f };
    float                    volume { 1.0f };
    bool                     visible { true };
    std::string              name;
    std::vector<std::string> sound;
    bool                     hasVolumeScript { false };
    std::string              volumeScript;
    std::string              volumeScriptProperties;

    bool FromJson(const nlohmann::json& json, fs::VFS&) {
        // Volume can be a plain number, a user-property object, or a scripted object
        if (json.contains("volume")) {
            auto& vol = json.at("volume");
            if (vol.is_number()) {
                volume = vol.get<float>();
            } else if (vol.is_object()) {
                volume = vol.value("value", 1.0f);
                if (vol.contains("script")) {
                    hasVolumeScript = true;
                    volumeScript = vol.at("script").get<std::string>();
                    if (vol.contains("scriptproperties"))
                        volumeScriptProperties = vol.at("scriptproperties").dump();
                }
            }
        }
        GET_JSON_NAME_VALUE(json, "playbackmode", playbackmode);
        GET_JSON_NAME_VALUE_NOWARN(json, "mintime", mintime);
        GET_JSON_NAME_VALUE_NOWARN(json, "maxtime", maxtime);
        GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
        GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
        if (! json.contains("sound") || ! json.at("sound").is_array()) {
            return false;
        }
        for (const auto& el : json.at("sound")) {
            std::string name;
            GET_JSON_VALUE(el, name);
            if (! name.empty()) sound.push_back(name);
        }
        return true;
    }
};
} // namespace wpscene
} // namespace wallpaper
