#pragma once
#include "WPPropertyAnimation.h"
#include "WPJson.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <cstdint>

namespace wallpaper
{
namespace wpscene
{

class WPMaterialPassBindItem {
public:
    bool        FromJson(const nlohmann::json&);
    std::string name;
    int32_t     index;
};

// One slot of a pass's `usertextures` array, index-aligned with `textures`.
// Shipped scenes use two shapes for a non-null entry: a bare string
// reference (e.g. "usertexture2", an app-launcher's custom-icon slot — `type`
// stays empty) and a `{"name":..., "type":...}` object (e.g.
// {"name":"$mediaThumbnail","type":"system"}, the now-playing album-art
// slot). A null entry (or the type default-constructed) parses to an empty
// `name`, matching how a null `textures` entry becomes an empty string:
// "no override for this slot".
class WPMaterialPassUserTexture {
public:
    bool        FromJson(const nlohmann::json&);
    std::string name;
    std::string type;
};

class WPMaterialPass {
public:
    bool                                                FromJson(const nlohmann::json&);
    void                                                Update(const WPMaterialPass&);
    std::vector<std::string>                            textures;
    std::vector<WPMaterialPassUserTexture>              usertextures;
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    // Keyframed constants, by the same name as their constantshadervalues entry.
    std::unordered_map<std::string, ConstantAnimation>  constantAnimations;
    std::string                                         target;
    std::vector<WPMaterialPassBindItem>                 bind;
};

class WPMaterial {
public:
    bool                                                FromJson(const nlohmann::json&);
    void                                                MergePass(const WPMaterialPass&);
    std::string                                         blending { "translucent" };
    std::string                                         cullmode { "nocull" };
    std::string                                         shader;
    std::string                                         depthtest { "disabled" };
    std::string                                         depthwrite { "disabled" };
    std::vector<std::string>                            textures;
    std::vector<WPMaterialPassUserTexture>              usertextures;
    std::unordered_map<std::string, int32_t>            combos;
    std::unordered_map<std::string, std::vector<float>> constantshadervalues;
    std::unordered_map<std::string, ConstantAnimation>  constantAnimations;

    // Maps user property name → shader constant name (from usershadervalues)
    std::unordered_map<std::string, std::string> userShaderBindings;

    bool use_puppet { false };
};

} // namespace wpscene
} // namespace wallpaper
