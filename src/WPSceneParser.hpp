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

    // Force scene.general.orthogonalprojection.postprocessing to a specific
    // value (e.g. "ultra") regardless of what the scene.json declares.  Empty
    // string means "no override, use the scene's own value".  Drives the HDR
    // bloom mip-chain on wallpapers that ship with hdr+bloom but no
    // postprocessing field.
    void SetPostprocessingOverride(const std::string& pp) { m_postprocessing_override = pp; }

private:
    std::string m_hide_pattern;
    std::string m_postprocessing_override;
};
} // namespace wallpaper
