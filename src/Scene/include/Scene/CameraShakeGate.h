#pragma once
#include <string_view>

namespace wallpaper
{

// Camera shake nudges the view-projection every frame so the whole picture
// sways.  Which cameras that reaches decides which parts of the wallpaper move:
// anything rendered through a camera that shows the global view has to sway
// with it, and anything rendering a layer into its own intermediate target must
// not (it would slide inside its own texture and get shifted a second time when
// that texture is composited).
//
// Split out as a pure decision so the rule can be exercised without a Vulkan
// device, and so the dirty gate and the apply site inside the uniform updater
// read the same answer — if they disagree, the cache hands a moving node the
// matrices it computed on some earlier frame.

// The scene-wide cameras.  An empty name means "the active camera".  A 2D scene
// pairs the ortho "global" with "global_perspective" for perspective particle
// systems; a 3D scene pairs the perspective "global" with the "global_ortho"
// overlay used by flat image layers.  All four frame the same view.
inline bool isGlobalViewCameraName(std::string_view cam_name) {
    return cam_name.empty() || cam_name == "global" || cam_name == "global_ortho" ||
           cam_name == "global_perspective";
}

// "effect" is the 2x2 ortho camera every post-processing pass draws its
// full-target quad through; it never frames the scene.
inline bool isPostProcessCameraName(std::string_view cam_name) { return cam_name == "effect"; }

// `linked_to_global_camera` is membership in the scene's list of cameras cloned
// from "global" each frame.  That list is how a compose effect's own camera --
// built at scene size and bolted onto the active camera's node -- says it rides
// the global view, and it is what separates those from the per-layer effect
// cameras, which carry an equally opaque name but frame a single layer.
inline bool cameraFollowsGlobalView(std::string_view cam_name, bool linked_to_global_camera) {
    if (isPostProcessCameraName(cam_name)) return false;
    return isGlobalViewCameraName(cam_name) || linked_to_global_camera;
}

} // namespace wallpaper
