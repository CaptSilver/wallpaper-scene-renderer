#pragma once
//
// Pure compose-helper for a SceneNode child's world transform when it
// optionally rigs into a parent puppet's named attachment slot.
//
// Extracted from WPSceneParser::ParseImageObj so the composition rule
// has a single home and can be unit-tested without spinning up a full
// scene.
//
// Algorithm (uniform at every depth):
//   - When the child's `attachment` name resolves on `parent_puppet`'s
//     MDAT to an attachment whose `bone_index` is a valid index into
//     the parent's bones array:
//         world = parent_world
//               * bones[bone_index].world_transform
//               * attachment.transform
//               * local
//     The bone's `world_transform` is the cumulative bind-pose world
//     populated by `WPPuppet::prepared()`.  For attachments rigged to
//     bone[0] of a flat skeleton it reduces to identity and the rule
//     collapses to `parent * att * local`; for attachments rigged to
//     non-root bones (head/hand/etc.) it lifts the child to the bone's
//     bind-pose position.
//   - Otherwise the standard scene-graph chain applies:
//         world = parent_world * local
//
// "Resolved" requires three things:
//   1. parent_puppet != nullptr
//   2. parent_puppet->findAttachment(name) != nullptr
//   3. attachment.bone_index is in range of parent_puppet->bones
//

#include "WPPuppet.hpp"

#include <Eigen/Geometry>
#include <memory>
#include <string_view>

namespace wallpaper
{

/// Result of attempting to compose an attached child's world transform.
struct AttachComposeResult {
    Eigen::Matrix4d world;             // The composed world transform.
    bool            attachment_resolved = false;
    // True when the named attachment was found AND its bone_index is in
    // range — i.e. the bone-anchored branch fired.  Useful in tests and
    // for diagnostic logs.
};

/// Drop only the translation column of a 4x4, keep the upper-3x3 (R*S).
inline Eigen::Matrix4d dropTranslation(const Eigen::Matrix4d& m) {
    Eigen::Matrix4d r = m;
    r(0, 3) = 0.0;
    r(1, 3) = 0.0;
    r(2, 3) = 0.0;
    return r;
}

/// Compose a child SceneNode's world transform.  Pure function; no I/O.
///
/// `parent_world`   — the parent SceneNode's world transform (already includes
///                    its own attachment chain, recursively).
/// `parent_puppet`  — the parent's puppet, or nullptr if the parent has no
///                    puppet (e.g. a plain image card).
/// `attachment_name`— the child's `attachment` field from scene.json (may
///                    be empty for un-attached children).
/// `local`          — the child's own T*R*S local transform (origin / scale /
///                    angles from scene.json), produced by SceneNode::GetLocalTrans.
inline AttachComposeResult composeAttachedChildWorld(
    const Eigen::Matrix4d&           parent_world,
    const std::shared_ptr<WPPuppet>& parent_puppet,
    std::string_view                 attachment_name,
    const Eigen::Matrix4d&           local) {
    AttachComposeResult result;
    if (parent_puppet && ! attachment_name.empty()) {
        if (auto* att = parent_puppet->findAttachment(attachment_name)) {
            if (att->bone_index < parent_puppet->bones.size()) {
                result.attachment_resolved = true;
                Eigen::Matrix4d bone_world =
                    parent_puppet->bones[att->bone_index]
                        .world_transform.matrix().cast<double>();
                Eigen::Matrix4d att_mat =
                    att->transform.matrix().cast<double>();
                // Uniform composition at every depth — keep child.local intact,
                // include the bone's cumulative bind-pose world.
                result.world =
                    parent_world * bone_world * att_mat * local;
                return result;
            }
        }
    }
    result.world = parent_world * local;
    return result;
}

} // namespace wallpaper
