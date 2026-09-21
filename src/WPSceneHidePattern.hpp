#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace wallpaper
{

// The comma-split/case-fold substring match behind sceneviewer's
// --hide-pattern flag: does `object_name` contain any of the
// comma-separated needles in `hide_pattern`?  Case-insensitive; an empty
// pattern or an empty needle segment (leading/trailing/double comma) never
// matches.  Extracted out of createImageNode's inline block (WPSceneParser.cpp)
// so the match rules can be pinned without a VFS or a parsed Scene.
inline bool HidePatternMatches(std::string_view hide_pattern, std::string_view object_name) {
    if (hide_pattern.empty()) return false;

    auto to_lower = [](std::string_view s) {
        std::string r(s);
        for (auto& c : r) c = (char)std::tolower((unsigned char)c);
        return r;
    };
    const std::string name_lower = to_lower(object_name);

    std::string_view needles(hide_pattern);
    while (! needles.empty()) {
        auto        comma        = needles.find(',');
        auto        needle       = needles.substr(0, comma);
        std::string needle_lower = to_lower(needle);
        if (! needle_lower.empty() && name_lower.find(needle_lower) != std::string::npos) {
            return true;
        }
        if (comma == std::string_view::npos) break;
        needles.remove_prefix(comma + 1);
    }
    return false;
}

} // namespace wallpaper
