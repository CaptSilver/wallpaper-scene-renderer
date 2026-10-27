#pragma once
#include "WPJson.hpp"
#include <nlohmann/json.hpp>
#include "WPMaterial.h"
#include <vector>
#include "WPPuppet.hpp"
#include "WPPropertyAnimation.h"
#include <unordered_set>
#include <string>
#include <filesystem>
#include <cctype>
#include <functional>
#include "SpecTexs.hpp"
#include "WPUserProperties.hpp"
#include "Utils/Logging.h"

namespace wallpaper
{
namespace fs
{
class VFS;
}

namespace wpscene
{

class WPEffectCommand {
public:
    bool        FromJson(const nlohmann::json&);
    std::string command;
    std::string target;
    std::string source;

    i32 afterpos { 0 }; // 0 for begin, start from 1
};

class WPEffectFbo {
public:
    bool        FromJson(const nlohmann::json&);
    std::string name;
    std::string format;
    uint32_t    scale { 1 };
};

class WPImageEffect {
private:
    static const std::unordered_set<std::string> BLACKLISTED_WORKSHOP_EFFECTS;
    bool                                         IsEffectBlacklisted(const std::string& filePath);

public:
    bool                         FromJson(const nlohmann::json&, fs::VFS& vfs);
    bool                         FromFileJson(const nlohmann::json&, fs::VFS& vfs);
    std::string                  name;
    bool                         visible { true };
    // Raw JSON of a `{"user": ...}`-bound `visible` field; empty when `visible`
    // was a plain literal (or the effect is blacklisted -- see FromJson).
    // `visible` above still holds the value resolved at parse time; this lets
    // assembleEffects re-resolve the binding at runtime without re-parsing
    // the scene.
    std::string                  userVisibleRawJson;
    // Script half of an effect-level {"script":...,"value":...} visible
    // field -- the idiom Wallpaper Engine's own "Now Playing" album-art
    // crossfade uses (Album Cover's Current/Previous passes). Empty when
    // visible was a plain bool or a {"user":...} binding.
    std::string                  visibleScript;
    std::string                  visibleScriptProperties; // JSON string, optional scriptproperties
    std::vector<WPMaterial>      materials;
    std::vector<WPMaterialPass>  passes;
    std::vector<WPEffectCommand> commands;
    std::vector<WPEffectFbo>     fbos;
    // Set on the effectpassthrough the parser appends to a link source so the
    // layer keeps drawing itself on screen while readers sample an isolated
    // copy of its chain output.  Not a scene.json field.
    bool synthesized_passthrough { false };
};

class WPImageObject {
public:
    struct Config {
        bool passthrough { false };
    };
    bool                 FromJson(const nlohmann::json&, fs::VFS&);
    int32_t              id { 0 };
    int32_t              parent_id { -1 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> size { 2.0f, 2.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    int32_t              colorBlendMode { 0 };
    float                alpha { 1.0f };
    float                brightness { 1.0f };
    bool                 fullscreen { false };
    // Model JSON "projectlayer": true — WE's "entire project as a layer"
    // primitive (models/util/projectlayer.json).  Its content is a
    // recapture of _rt_default every tick, so its effect-chain RT must
    // track the render output like a fullscreen layer, not its authored
    // scene.json size.  Kept separate from `fullscreen`: that flag is also
    // read by the no-effect script-host placeholder branch in
    // ParseImageObj, and a zero-effect project layer sharing it would be
    // wrongly routed into the "never rendered" path.
    bool projectlayer { false };
    bool nopadding { false };
    // Set when the image-model JSON declares "autosize": true.  Resolved in
    // ParseImageObj by reading the first texture's sprite-frame dimensions
    // (or mapWidth × mapHeight for non-sprite textures).
    bool autosize { false };
    // Model JSON "solidlayer": true — a coloured quad with no texture, drawn
    // by WE's flat shader from the object's own color and alpha.  Authors use
    // them both as painted backgrounds and, with alpha 0, as invisible group
    // anchors; the renderer draws exactly what is authored either way.
    bool solidlayer { false };
    // Scene-level "copybackground": true (default) means a passthrough compose
    // layer captures _rt_default into its pingpong before running its effect
    // chain.  When false, the effect chain runs on whatever the scene graph
    // has already written into the pingpong (e.g. children rendered there) —
    // suppress the implicit screen copy in SceneToRenderGraph's passthrough
    // path so the pingpong isn't overwritten.  Default true matches WE
    // semantics for composelayers.
    bool copybackground { true };
    bool visible { true };
    bool visibleIsComboSelector { false }; // combo condition-based visibility (skip offscreen)
    // The user property `visible` is bound to, if any.
    std::string visibleUserProp;
    // WE editor flag: when true, this object doesn't inherit transform /
    // visibility / alpha / tint from parent groups or nodes.  Parsed for
    // completeness; currently no group hierarchy exists in the scenes we
    // handle, so nothing to propagate.
    bool        disablepropagation { false };
    bool        perspective { false }; // Use perspective camera (default: flat/ortho)
    // 360° skybox flag from the image's model JSON or scene-level override.
    // When true, Scene::has_skybox is set in ParseImageObj and the layer id is
    // appended to Scene::skyboxLayerIds.  Detection-only today; full cubemap
    // render path (depth-disabled fullscreen pass sampling by
    // inverse(viewProj)*clipPos worldDir) is deferred — see Scene::has_skybox
    // documentation.
    bool        is_skybox { false };
    std::string image;
    std::string alignment { "center" };
    WPMaterial  material;
    std::vector<WPImageEffect> effects;
    Config                     config;

    std::string                                puppet;
    std::vector<WPPuppetLayer::AnimationLayer> puppet_layers;

    // Parent-puppet attachment point name (scene.json "attachment" field).
    // When non-empty and the parent is a puppet, this child anchors to the
    // named MDAT attachment in the parent puppet's skeleton instead of the
    // parent mesh center.  e.g. hair pieces with attachment="head" end up
    // at the head anchor in asuna body's skeleton.
    std::string attachment;

    // Keyframe animations parsed out of <prop>.animation blocks.
    // Each entry targets a specific scalar property on this object (e.g. alpha).
    std::vector<PropertyAnimation> propertyAnimations;

    // Color property script (e.g. audio-reactive color)
    std::string colorScript;
    std::string colorScriptProperties; // JSON string

    // scene.json "dependencies": [id, ...] — set on compose layers that sample
    // the listed images via `_rt_imageLayerComposite_<id>_a`.  WPSceneParser
    // collects these across all compose layers and forces every referenced
    // image to render offscreen so the compose layer can sample an isolated
    // sprite RT instead of getting a full-FB snapshot via _rt_link_<id>.
    // Without this, images that are intended to be sampled by a parent compose
    // blend ALSO paint themselves onto _rt_default — and the compose layer's
    // blend then produces solid quads (each Calque CO33-Mx in Clair Obscur
    // Expedition 33 3498984739 produced a gray rectangle over its character).
    // Only compose layers hide their dependencies this way; any other layer's
    // list just marks the ids as read (see CollectImageLinkReaderIds).
    std::vector<int32_t> dependencies;
};

// Markers recognising the two model paths WE treats as a full compose blend
// ("models/util/composelayer.json" and "models/util/projectlayer.json"
// stored in WPImageObject::image) — see synthesizePassthroughForCompose in
// WPSceneParser.cpp for why projectlayer counts too: it's WE's "entire
// project as a layer" primitive and uses the same compose material.  Only
// these two model kinds sample sibling layer RTs via
// _rt_imageLayerComposite_<id>_a through their `dependencies` list, so
// they're the only layers whose `dependencies` should force dependents
// offscreen on that basis alone.
constexpr std::string_view kComposeLayerImage = "models/util/composelayer.json";
constexpr std::string_view kProjectLayerImage = "models/util/projectlayer.json";

inline bool IsComposeLayerImage(std::string_view image) {
    return image == kComposeLayerImage || image == kProjectLayerImage;
}

// The user property an effect's `"visible": {"user": ...}` binding names, or
// empty if the stored JSON is missing, malformed or names nothing.
inline std::string EffectUserPropName(const WPImageEffect& effect) {
    if (effect.userVisibleRawJson.empty()) return {};
    try {
        return UserPropertyNameOf(nlohmann::json::parse(effect.userVisibleRawJson));
    } catch (const std::exception& e) {
        LOG_ERROR("effect user-bound visible JSON failed to re-parse: %s", e.what());
        return {};
    }
}

// Whether an effect gets added to its layer: it is visible, or carries a
// binding that resolves (a named user property or a visible.script) so a later
// toggle can show it without a reload.  On a layer a SceneScript reaches with
// getEffect(...).visible every effect builds.
inline bool EffectWouldBuild(const WPImageEffect& e, bool layerScriptTargeted) {
    return e.visible || layerScriptTargeted || ! EffectUserPropName(e).empty() ||
           ! e.visibleScript.empty();
}

// Decides per layer whether a SceneScript can toggle its effects.  Empty means no.
template<typename Obj>
using LayerScriptTargetFn = std::function<bool(const Obj&)>;

// Scan a material's texture list for a raw `_rt_imageLayerComposite_<id>`
// reference and record <id> in `out`.  This is the OTHER way a layer can
// end up depending on a sibling's composite RT: ParseSpecTexName resolves
// this texture name at render time regardless of whether <id> ever appears
// in anyone's scene.json `dependencies` array, so a hand-authored or
// malformed material that spells out the name directly still needs its
// source forced offscreen.
inline void collectComposeLinkIdsFromTextures(const std::vector<std::string>& textures,
                                              int32_t self_id, std::unordered_set<int32_t>& out) {
    for (const auto& tex : textures) {
        if (! sstart_with(tex, WE_IMAGE_LAYER_COMPOSITE_PREFIX)) continue;
        std::string_view rest =
            std::string_view(tex).substr(WE_IMAGE_LAYER_COMPOSITE_PREFIX.size());
        size_t ndigits = 0;
        while (ndigits < rest.size() && std::isdigit(static_cast<unsigned char>(rest[ndigits])))
            ++ndigits;
        if (ndigits == 0) continue;
        int32_t dep_id = -1;
        STRTONUM(std::string(rest.substr(0, ndigits)), dep_id);
        if (dep_id < 0 || dep_id == self_id) continue;
        out.insert(dep_id);
    }
}

// Texture strings an effect reads: its materials' own and the scene.json pass
// overrides, which assembleEffects merges over the materials.
inline void collectEffectComposeLinkIds(const WPImageEffect& effect, int32_t self_id,
                                        std::unordered_set<int32_t>& out) {
    for (const auto& mat : effect.materials)
        collectComposeLinkIdsFromTextures(mat.textures, self_id, out);
    for (const auto& pass : effect.passes)
        collectComposeLinkIdsFromTextures(pass.textures, self_id, out);
}

// Collect the image ids that any *compose* layer references in its
// `dependencies` list.  These are the only sources forced offscreen on that
// basis alone.  Two filters apply:
//
//   1) Only image objects whose `image` is a compose-layer model
//      contribute — non-compose layers that happen to declare
//      `dependencies` (e.g. authoring quirks) must NOT force their
//      "dependents" offscreen, because no compose-blend will ever read
//      the resulting offscreen RT and the routed pixels would be lost.
//      Driver: Eclipse 1210462523 — a single non-compose image layer
//      declared `dependencies:[64,64,64]` (its own id three times).
//      Without this filter, the layer was forced to `_rt_offscreen_64`,
//      nothing read it, and the screen stayed at clearColor.
//
//   2) Self-references are dropped — a layer can't sample its own
//      output via a link RT.  Same Eclipse 1210462523 case above.
//
// A layer read through a texture string, or listed by a non-compose layer, is
// not hidden: see CollectImageLinkReaderIds.
//
// Header-resident as a small free function so the test suite can pin
// these two filters without spinning up the full WPSceneParser::Parse
// pipeline.
inline std::unordered_set<int32_t>
CollectComposeDependencyIds(const std::vector<const WPImageObject*>& image_objs) {
    std::unordered_set<int32_t> out;
    for (const WPImageObject* img : image_objs) {
        if (img == nullptr || ! IsComposeLayerImage(img->image)) continue;
        for (int32_t dep_id : img->dependencies) {
            if (dep_id == img->id) continue;
            out.insert(dep_id);
        }
    }
    return out;
}

// Collect the image ids that an image layer reads as an ordinary reader: a raw
// `_rt_imageLayerComposite_<id>` texture string in its base material, or in
// the materials or scene.json pass overrides of an effect that will be built,
// plus the `dependencies` of a non-compose layer (the editor writes it on
// every reader).  Self-references are dropped.
//
// An effect that will not be built never reaches LoadMaterial, so its strings
// never resolve; `layerScriptTargeted` says whether a script can show any of a
// layer's effects (every effect then builds).
//
// A source named here keeps drawing itself and gives readers an isolated copy
// of its chain output.  Only compose `dependencies` send a source offscreen.
inline std::unordered_set<int32_t>
CollectImageLinkReaderIds(const std::vector<const WPImageObject*>&  image_objs,
                          const LayerScriptTargetFn<WPImageObject>& layerScriptTargeted = {}) {
    std::unordered_set<int32_t> out;
    for (const WPImageObject* img : image_objs) {
        if (img == nullptr) continue;
        if (! IsComposeLayerImage(img->image)) {
            for (int32_t dep_id : img->dependencies) {
                if (dep_id != img->id) out.insert(dep_id);
            }
        }
        collectComposeLinkIdsFromTextures(img->material.textures, img->id, out);
        const bool targeted = layerScriptTargeted && layerScriptTargeted(*img);
        for (const auto& effect : img->effects) {
            if (! EffectWouldBuild(effect, targeted)) continue;
            collectEffectComposeLinkIds(effect, img->id, out);
        }
    }
    return out;
}

// Resolve the panorama texture key for a skybox-tagged image object: its first
// material texture, matching the imgIdToSourceTexture convention.  Returns ""
// when there is no base texture, the base is empty, or the base is a spec
// render-target (`_rt_` prefix) — none of which is a samplable panorama.  The
// skybox background pass reads this key; an empty return means no pass is
// emitted.  Header-resident so the resolution can be pinned without standing up
// the full WPSceneParser pipeline.
inline std::string resolveSkyboxTexKey(const WPImageObject& obj) {
    if (obj.material.textures.empty()) return {};
    const std::string& base = obj.material.textures.front();
    if (base.empty()) return {};
    // Spec RTs start with "_rt_"; they are pipeline scratch, never a panorama.
    if (base.rfind("_rt_", 0) == 0) return {};
    return base;
}

} // namespace wpscene
} // namespace wallpaper
