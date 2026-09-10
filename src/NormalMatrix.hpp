#pragma once
#include <Eigen/Dense>

namespace wallpaper
{

// Normals do not transform by the model matrix.  Under a non-uniform scale the
// model matrix shears them off the surface, so they need the inverse transpose
// of its rotation/scale part.
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
    const double det = linear.determinant();
    Eigen::Matrix3d normal =
        std::abs(det) > 1e-12 ? Eigen::Matrix3d(linear.inverse().transpose()) : linear;

    Eigen::Matrix4d out    = Eigen::Matrix4d::Identity();
    out.topLeftCorner<3, 3>() = normal;
    return out;
}

} // namespace wallpaper
