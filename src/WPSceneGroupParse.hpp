#pragma once
//
// Pure parse-helper for a "group" object in scene.json.
//
// A group is a transform-only container: it carries origin/scale/angles
// and a `visible` flag, but no renderable content (no image/particle/
// text/sound/light field).  Children parented to a hidden group
// disappear via the SceneNode parent-walk in IsVisible().
//
// Extracted from WPSceneParser so a single home parses all group
// fields (including `visible`, which was previously missed — letting
// Solar System's p4cloud group erroneously render its image child).
//

#include "Scene/SceneNode.h"
#include "WPJson.hpp"

#include <Eigen/Core>
#include <nlohmann/json.hpp>

#include <array>
#include <memory>

namespace wallpaper
{

struct GroupParseResult {
    std::shared_ptr<SceneNode> node;
    i32                        id        = 0;
    i32                        parent_id = -1;
    bool                       visible   = true;
    // Authored parallaxDepth on the group itself.  Children that don't set
    // their own parallaxDepth inherit this default (WE semantic: groups can
    // declare a "depth plane" that all descendants share).  Driver:
    // 3363252053 audio-bars group (id 1115) sets parallaxDepth=(0.06, 0.06)
    // with three solidlayer children that don't set their own.
    std::array<float, 2>       parallaxDepth { 0.0f, 0.0f };
    bool                       hasParallaxDepth { false };
};

// Parse a single group JSON object into a SceneNode + linkage info.
// Caller is responsible for inserting `node` into the scene graph
// (attach to parent_id or scene root in a second pass).
inline GroupParseResult ParseGroupNode(const nlohmann::json& obj) {
    using Eigen::Vector3f;
    GroupParseResult r;
    GET_JSON_NAME_VALUE(obj, "id", r.id);

    std::array<float, 3> origin { 0, 0, 0 };
    std::array<float, 3> scale { 1, 1, 1 };
    std::array<float, 3> angles { 0, 0, 0 };
    GET_JSON_NAME_VALUE_NOWARN(obj, "origin", origin);
    GET_JSON_NAME_VALUE_NOWARN(obj, "scale", scale);
    GET_JSON_NAME_VALUE_NOWARN(obj, "angles", angles);
    GET_JSON_NAME_VALUE_NOWARN(obj, "visible", r.visible);
    GET_JSON_NAME_VALUE_NOWARN(obj, "parent", r.parent_id);
    if (obj.contains("parallaxDepth")) {
        r.hasParallaxDepth =
            GET_JSON_NAME_VALUE_NOWARN(obj, "parallaxDepth", r.parallaxDepth);
    }

    r.node = std::make_shared<SceneNode>(
        Vector3f(origin.data()), Vector3f(scale.data()), Vector3f(angles.data()));
    r.node->ID() = r.id;
    // Group nodes propagate visibility to descendants: a hidden group
    // hides every renderable beneath it via the parent-walk in
    // SceneNode::IsVisible().
    r.node->SetVisible(r.visible);

    return r;
}

} // namespace wallpaper
