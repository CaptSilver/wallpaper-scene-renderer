#include "CameraLayer.hpp"

#include <sstream>

using namespace wallpaper;

namespace
{

// Scene JSON stores vectors as "x y z" strings.  A scripted property wraps the
// literal in {"script": ..., "value": "x y z"}; the value is the seed we want.
bool ReadVec3(const nlohmann::json& obj, const char* key, std::array<float, 3>& out) {
    if (! obj.contains(key)) return false;
    const nlohmann::json* v = &obj.at(key);
    if (v->is_object()) {
        if (! v->contains("value")) return false;
        v = &v->at("value");
    }
    if (! v->is_string()) return false;

    std::istringstream in(v->get<std::string>());
    std::array<float, 3> parsed { 0.0f, 0.0f, 0.0f };
    for (auto& f : parsed) {
        if (! (in >> f)) return false;
    }
    out = parsed;
    return true;
}

} // namespace

SceneCameraLayer wallpaper::FindCameraLayer(const nlohmann::json& objects) {
    if (! objects.is_array()) return {};

    for (const auto& obj : objects) {
        if (! obj.is_object()) continue;
        if (! obj.contains("camera") || ! obj.at("camera").is_string()) continue;

        SceneCameraLayer layer;
        layer.found = true;
        ReadVec3(obj, "origin", layer.origin);
        ReadVec3(obj, "angles", layer.angles);
        if (obj.contains("fov") && obj.at("fov").is_number())
            layer.fov = obj.at("fov").get<float>();
        return layer;
    }
    return {};
}

CameraBasis wallpaper::CameraBasisFromLayer(const SceneCameraLayer& layer) {
    // Same euler order the scene graph uses for node rotation: X, then Y, then
    // Z, composed as Rz * Ry * Rx.
    const double cx = std::cos(layer.angles[0]), sx = std::sin(layer.angles[0]);
    const double cy = std::cos(layer.angles[1]), sy = std::sin(layer.angles[1]);
    const double cz = std::cos(layer.angles[2]), sz = std::sin(layer.angles[2]);

    // Columns of Rz * Ry * Rx.
    const double m[3][3] = {
        { cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx },
        { sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx },
        { -sy, cy * sx, cy * cx },
    };

    auto rotate = [&m](double x, double y, double z) {
        return std::array<double, 3> { m[0][0] * x + m[0][1] * y + m[0][2] * z,
                                       m[1][0] * x + m[1][1] * y + m[1][2] * z,
                                       m[2][0] * x + m[2][1] * y + m[2][2] * z };
    };

    // No angles means identity orientation, i.e. looking down -Z.  The saved
    // editor block's `center` must not be borrowed here — it encodes a
    // different eye position and would tilt the view.
    const auto forward = rotate(0.0, 0.0, -1.0);
    const auto up      = rotate(0.0, 1.0, 0.0);

    CameraBasis basis;
    basis.eye = { (double)layer.origin[0], (double)layer.origin[1], (double)layer.origin[2] };
    for (int i = 0; i < 3; i++) basis.center[i] = basis.eye[i] + forward[i];
    basis.up = up;
    return basis;
}
