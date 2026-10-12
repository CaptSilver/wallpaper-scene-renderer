#include "WPAnimationJson.h"

#include "WPJson.hpp"

#include <array>

namespace wallpaper::wpscene
{

// WE keeps the options on `animation.options`, except `relative`, which sits
// on the animation object itself.  Scalar and vec3 tracks want the identical
// set, and while each parser spelled the block out by hand the scalar one
// quietly missed `wraploop`: every alpha animation held its last keyframe and
// snapped at the loop boundary.
void ParseAnimationOptions(const nlohmann::json& animJson, PropertyAnimation& out) {
    if (animJson.contains("options") && animJson.at("options").is_object()) {
        const auto& opts = animJson.at("options");
        GET_JSON_NAME_VALUE_NOWARN(opts, "name", out.name);
        GET_JSON_NAME_VALUE_NOWARN(opts, "fps", out.fps);
        GET_JSON_NAME_VALUE_NOWARN(opts, "length", out.length);
        std::string mode;
        GET_JSON_NAME_VALUE_NOWARN(opts, "mode", mode);
        out.mode = ParsePropertyAnimMode(mode);
        GET_JSON_NAME_VALUE_NOWARN(opts, "startpaused", out.startPaused);
        GET_JSON_NAME_VALUE_NOWARN(opts, "wraploop", out.wraploop);
    }
    GET_JSON_NAME_VALUE_NOWARN(animJson, "relative", out.relative);
}

std::optional<ConstantAnimation> ParseConstantAnimation(const nlohmann::json&     field,
                                                        const std::vector<float>& base) {
    if (! field.is_object() || ! field.contains("animation")) return std::nullopt;
    const auto& animJson = field.at("animation");
    if (! animJson.is_object()) return std::nullopt;

    PropertyAnimation shared {};
    ParseAnimationOptions(animJson, shared);
    shared.playing = ! shared.startPaused;

    constexpr std::array<const char*, 4> kChannelKeys { "c0", "c1", "c2", "c3" };
    ConstantAnimation                    out;
    out.value = base;
    for (usize ch = 0; ch < kChannelKeys.size(); ch++) {
        if (! animJson.contains(kChannelKeys[ch])) continue;
        const auto& keys = animJson.at(kChannelKeys[ch]);
        if (! keys.is_array() || keys.empty()) continue;
        PropertyAnimation track = shared;
        for (const auto& k : keys) {
            PropertyAnimKeyframe kf {};
            GET_JSON_NAME_VALUE_NOWARN(k, "frame", kf.frame);
            GET_JSON_NAME_VALUE_NOWARN(k, "value", kf.value);
            track.keyframes.push_back(kf);
        }
        if (out.value.size() <= ch) out.value.resize(ch + 1, 0.0f);
        track.initialValue = out.value[ch];
        out.tracks.emplace_back(ch, std::move(track));
    }
    if (out.tracks.empty()) return std::nullopt;
    return out;
}

} // namespace wallpaper::wpscene
