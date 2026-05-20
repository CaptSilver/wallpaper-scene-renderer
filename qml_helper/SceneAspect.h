#pragma once

namespace scenebackend
{

// Native aspect ratio (width / height) of a parsed scene's orthographic size.
// Split out of SceneObject::nativeAspectRatio() so the load-gate sentinel and
// the divide-by-zero guard can be unit-tested without constructing the
// Vulkan-backed QQuickItem (whose ortho members are only filled by a full
// scene load).
//
// Returns 0.0 as a sentinel meaning "no usable aspect yet": either the scene
// has not finished loading (loaded == false) or the height is non-positive (a
// degenerate ortho size).  The plugin's Scene.qml treats 0.0 as "fall back to
// anchors.fill"; any positive value sizes the renderer item to that aspect so
// the wrapper's BackgroundColor Rectangle shows through the letterbox bars.
//
// Width is assumed non-negative (a parsed ortho size never carries a negative
// extent); a zero width collapses to the same 0.0 sentinel, which the QML
// fill-fallback handles identically.
inline double computeNativeAspectRatio(bool loaded, float orthoW, float orthoH) {
    if (! loaded || orthoH <= 0.0f) return 0.0;
    return static_cast<double>(orthoW) / static_cast<double>(orthoH);
}

} // namespace scenebackend
