#pragma once
// Reading WE's keyframe animation blocks out of scene JSON:
//   "<prop>": { "animation": { "c0":[{frame,value,…}], …, "options":{…} }, "value": <base> }

#include "WPPropertyAnimation.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <vector>

namespace wallpaper::wpscene
{

// Reads the playback options every keyframe animation shares into `out`.
void ParseAnimationOptions(const nlohmann::json& animJson, PropertyAnimation& out);

// An effect constant written as an animation block: one track per authored
// channel (c0..c3), each starting from the matching component of `base`, the
// constant's already-parsed `value`. nullopt when `field` carries no animation
// or none of its channels has keyframes.
std::optional<ConstantAnimation> ParseConstantAnimation(const nlohmann::json&     field,
                                                        const std::vector<float>& base);

} // namespace wallpaper::wpscene
