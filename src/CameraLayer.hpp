#pragma once
#include <array>
#include <cmath>
#include <nlohmann/json.hpp>

namespace wallpaper
{

// A scene object carrying `"camera": "default"`.  Wallpaper Engine stores the
// runtime camera as a layer in the object list; the scene-level `camera` block
// is the editor's saved viewport and can differ.
struct SceneCameraLayer {
    bool                found { false };
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    float               fov { 0.0f };
};

// Eye/center/up triple in the form SetDirectLookAt wants.
struct CameraBasis {
    std::array<double, 3> eye { 0.0, 0.0, 0.0 };
    std::array<double, 3> center { 0.0, 0.0, -1.0 };
    std::array<double, 3> up { 0.0, 1.0, 0.0 };
};

SceneCameraLayer FindCameraLayer(const nlohmann::json&);

// A camera layer without a usable fov falls back to the scene's.  Zero is the
// sentinel for "absent" rather than a legitimate degenerate frustum.
inline float CameraFovFor(const SceneCameraLayer& layer, float scene_fov) {
    return (layer.found && layer.fov > 0.0f) ? layer.fov : scene_fov;
}

CameraBasis CameraBasisFromLayer(const SceneCameraLayer&);

} // namespace wallpaper
