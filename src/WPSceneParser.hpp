#pragma once
#include "Interface/ISceneParser.h"
#include <random>

namespace wallpaper
{

class WPSceneParser : public ISceneParser {
public:
    WPSceneParser()  = default;
    ~WPSceneParser() = default;
    std::shared_ptr<Scene> Parse(std::string_view scene_id, const std::string&, fs::VFS&,
                                 audio::SoundManager&, const WPUserProperties& userProps) override;

    // Debug: hide any object whose name contains any comma-separated needle.
    // Intended for sceneviewer's --hide-pattern CLI flag.
    void SetHidePattern(const std::string& pat) { m_hide_pattern = pat; }

private:
    std::string m_hide_pattern;
};
} // namespace wallpaper
