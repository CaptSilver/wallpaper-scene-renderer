#pragma once
#include <Eigen/Dense>
#include <cmath>
#include <algorithm>

namespace wallpaper::vulkan
{

// Map a world-space view direction to equirectangular panorama UV, mirroring
// the skybox fragment shader exactly so the CPU-side unit tests pin the same
// math the GPU runs:
//   u = atan2(dir.x, dir.z) / (2π) + 0.5
//   v = acos(clamp(dir.y, -1, 1)) / π
// dir.y is clamped into acos's domain so a non-normalized input can't NaN.
// The direction need not be normalized for u (atan2 is scale-invariant); for a
// meaningful v the caller should pass a unit-length dir, but the clamp keeps a
// degenerate one finite.
inline Eigen::Vector2f equirectUV(const Eigen::Vector3f& dir) {
    constexpr float kPi     = 3.14159265358979323846f;
    constexpr float kInv2Pi = 1.0f / (2.0f * kPi);
    constexpr float kInvPi  = 1.0f / kPi;

    const float u = std::atan2(dir.x(), dir.z()) * kInv2Pi + 0.5f;
    const float y = std::clamp(dir.y(), -1.0f, 1.0f);
    const float v = std::acos(y) * kInvPi;
    return Eigen::Vector2f { u, v };
}

} // namespace wallpaper::vulkan
