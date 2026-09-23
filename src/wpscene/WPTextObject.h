#pragma once
#include "WPJson.hpp"
#include "WPImageObject.h"
#include <nlohmann/json.hpp>
#include <array>
#include <string>
#include <vector>
#include <unordered_set>

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
    // True when `origin` is a property script (e.g. 3363252053's clock layers
    // compute absolute scene coords as `scriptProperties.x * canvasSize.x`).
    // Such layers must NOT inherit their parent's transform on the effect
    // composite — the script already produces world-space coords, so re-applying
    // the parent chain double-shifts them.  Static-origin text-effect layers
    // (e.g. 3122339805's world clock) DO need the parent transform restored.
    bool                 originIsScripted { false };
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

// Collect every image id a text layer's effect chain reads via a raw
// `_rt_imageLayerComposite_<id>` texture string in its own effect
// materials.  A text layer is never itself a compose-blend target
// (composelayer.json / projectlayer.json only apply to image objects) but
// it can still be a *reader* of another layer's composite RT: text
// effects go through the same LoadMaterial/ParseSpecTexName path as image
// effects, so the reference is honored at render time whether or not the
// source id ever appears in anyone's scene.json `dependencies` array.
inline std::unordered_set<int32_t>
CollectTextComposeLinkIds(const std::vector<const WPTextObject*>& text_objs) {
    std::unordered_set<int32_t> out;
    for (const WPTextObject* txt : text_objs) {
        if (txt == nullptr) continue;
        for (const auto& effect : txt->effects) {
            // Same invisible-effect exclusion as the image-object scan in
            // WPImageObject.h: an invisible effect never reaches LoadMaterial
            // at render time, so its texture strings never resolve.
            if (! effect.visible) continue;
            for (const auto& mat : effect.materials)
                collectComposeLinkIdsFromTextures(mat.textures, txt->id, out);
        }
    }
    return out;
}

} // namespace wpscene
} // namespace wallpaper
