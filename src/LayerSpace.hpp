#pragma once
#include <unordered_map>
#include <unordered_set>

#include "Core/Literals.hpp"

namespace wallpaper
{

// Flat image layers in a perspective scene composite through the pixel-space
// ortho overlay camera, so SceneScript origins in [-0.5, 0.5] land where the
// author expects.  A layer parented to a 3D model is not flat: it inherits a
// world transform in scene units, and projecting that through a pixel-space
// window scales it by the parent's 3D scale.  A sun-glare sprite parented to a
// model at scale 10.92 then covers the whole framebuffer.
//
// Walks the declared-parent chain rather than checking only the immediate
// parent, since layers are commonly nested under a group that is itself
// parented to the model.  `parent_of` maps object id to its declared parent
// (absent, or negative, means the scene root).
inline bool InheritsModelSpace(i32 parent_id, const std::unordered_set<i32>& model_ids,
                               const std::unordered_map<i32, i32>& parent_of) {
    // Authored hierarchies are shallow; the bound only exists so a malformed
    // scene with a parent cycle cannot hang the parser.
    constexpr int max_depth = 64;

    i32 id = parent_id;
    for (int depth = 0; depth < max_depth && id >= 0; depth++) {
        if (model_ids.count(id)) return true;
        auto it = parent_of.find(id);
        if (it == parent_of.end()) return false;
        id = it->second;
    }
    return false;
}

} // namespace wallpaper
