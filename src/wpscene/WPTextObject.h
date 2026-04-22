#pragma once
#include "WPJson.hpp"
#include "WPImageObject.h"
#include <nlohmann/json.hpp>
#include <array>
#include <string>
#include <vector>

namespace wallpaper
{
namespace fs
{
class VFS;
}

namespace wpscene
{

class WPTextObject {
public:
    bool FromJson(const nlohmann::json&, fs::VFS&);

    int32_t              id { 0 };
    int32_t              parent_id { -1 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> size { 2.0f, 2.0f };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    float                alpha { 1.0f };
    float                brightness { 1.0f };
    int32_t              colorBlendMode { 0 };
    bool                 visible { true };

    std::string font;
    float       pointsize { 32.0f };
    std::string horizontalalign { "center" };
    std::string verticalalign { "center" };
    std::string anchor { "none" }; // WE bbox-anchor field, separate from h/valign
    int32_t     padding { 0 };
    // maxwidth: the author-declared soft word-wrap boundary (in scene units)
    // for autosize text.  Used as the canvas width so final rendered width
    // matches WE semantics — multi-line wrapping happens inside this box.
    float   maxwidth { 0.0f };
    int32_t maxrows { 0 }; // 0 = unlimited; wallpaper 2866203962 sets 1 for track name

    std::string textValue;            // from text.value
    std::string textScript;           // from text.script
    std::string textScriptProperties; // from text.scriptproperties (JSON)
    std::string pointsizeUserProp;    // user property name controlling pointsize

    std::vector<WPImageEffect> effects;
};

} // namespace wpscene
} // namespace wallpaper
