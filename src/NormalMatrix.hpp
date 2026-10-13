#pragma once
#include <Eigen/Dense>
#include <cmath>

namespace wallpaper
{

// Normals do not transform by the model matrix.  Under a non-uniform scale the
// model matrix shears them off the surface, so they need the inverse transpose
// of its rotation/scale part.
//
// The result is divided back to unit determinant so it carries the shape of the
// scale but not its size.  Stock shaders transform tangents and bitangents with
// this matrix and never renormalize them; a raw inverse transpose of a model
// drawn at scale 0.1 makes them 10x the normal and the normal map's tilt 10x
// too steep.  Normalizing each column instead would collapse a non-uniform
// scale to the bare rotation and lose the correction this matrix exists for.
//
// Returned as a 4x4 with the normal matrix in the top-left 3x3.  The uniform
// upload clamps a mat4 down to std140's padded mat3 layout, which places the
// columns at 16-byte strides; writing a tightly packed 3x3 directly would land
// columns 1 and 2 at offsets 12 and 24 instead of 16 and 32.
inline Eigen::Matrix4d NormalMatrixFrom(const Eigen::Matrix4d& model) {
    Eigen::Matrix3d linear = model.topLeftCorner<3, 3>();

    // A degenerate linear part (a zero scale on some axis) has no inverse;
    // fall back to the matrix itself so the upload stays finite instead of
    // filling the uniform with NaN.
    const double    det    = linear.determinant();
    Eigen::Matrix3d normal = linear;
    if (std::abs(det) > 1e-12) {
        // det(inverse transpose) = 1/det, so this scales it to determinant +-1.
        normal = linear.inverse().transpose() * std::cbrt(std::abs(det));
    }

    Eigen::Matrix4d out       = Eigen::Matrix4d::Identity();
    out.topLeftCorner<3, 3>() = normal;
    return out;
}

} // namespace wallpaper
