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

struct VolumeKeyframe {
    float frame { 0 };
    float value { 0 };
};

struct VolumeAnimation {
    std::string                  name;
    std::string                  mode { "loop" };
    float                        fps { 30.0f };
    float                        length { 0 };
    std::vector<VolumeKeyframe>  keyframes;
};

struct WPSoundObject {
    std::string              playbackmode { "loop" };
    float                    maxtime { 10.0f };
    float                    mintime { 0.0f };
    float                    volume { 1.0f };
    bool                     visible { true };
    bool                     startsilent { false };
    std::string              name;
    std::vector<std::string> sound;
    bool                     hasVolumeScript { false };
    std::string              volumeScript;
    std::string              volumeScriptProperties;
    bool                     hasVolumeAnimation { false };
    VolumeAnimation          volumeAnimation;

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
                if (vol.contains("animation")) {
                    auto& anim = vol.at("animation");
                    if (anim.contains("c0") && anim.at("c0").is_array()) {
                        hasVolumeAnimation = true;
                        for (const auto& kf : anim.at("c0")) {
                            VolumeKeyframe vk;
                            vk.frame = kf.value("frame", 0.0f);
                            vk.value = kf.value("value", 0.0f);
                            volumeAnimation.keyframes.push_back(vk);
                        }
                        if (anim.contains("options")) {
                            auto& opts = anim.at("options");
                            volumeAnimation.fps    = opts.value("fps", 30.0f);
                            volumeAnimation.length = opts.value("length", 0.0f);
                            volumeAnimation.mode   = opts.value("mode", "loop");
                            volumeAnimation.name   = opts.value("name", "");
                        }
                    }
                }
            }
        }
        GET_JSON_NAME_VALUE(json, "playbackmode", playbackmode);
        GET_JSON_NAME_VALUE_NOWARN(json, "mintime", mintime);
        GET_JSON_NAME_VALUE_NOWARN(json, "maxtime", maxtime);
        GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
        GET_JSON_NAME_VALUE_NOWARN(json, "startsilent", startsilent);
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
