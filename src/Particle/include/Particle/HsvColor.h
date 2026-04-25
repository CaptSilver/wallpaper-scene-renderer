#pragma once

#include <Eigen/Core>
#include <cmath>

namespace wallpaper
{

// Six-sextant HSV→RGB conversion.  `h` is in degrees [0, 360); `s` and `v`
// in [0, 1].  Out-of-range hue is wrapped via fmod; out-of-range s/v is
// clamped to [0, 1] so an author who jitters past the boundary doesn't
// produce negative or >1 RGB.
inline Eigen::Vector3d HsvToRgb(double h, double s, double v) noexcept {
    h = std::fmod(h, 360.0);
    if (h < 0.0) h += 360.0;
    if (s < 0.0) s = 0.0;
    if (s > 1.0) s = 1.0;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;

    const double c = v * s;
    const double sextant = h / 60.0;
    const double x = c * (1.0 - std::fabs(std::fmod(sextant, 2.0) - 1.0));
    const double m = v - c;

    double r = 0, g = 0, b = 0;
    if (sextant < 1.0) {
        r = c;  g = x;  b = 0;
    } else if (sextant < 2.0) {
        r = x;  g = c;  b = 0;
    } else if (sextant < 3.0) {
        r = 0;  g = c;  b = x;
    } else if (sextant < 4.0) {
        r = 0;  g = x;  b = c;
    } else if (sextant < 5.0) {
        r = x;  g = 0;  b = c;
    } else {
        r = c;  g = 0;  b = x;
    }
    return { r + m, g + m, b + m };
}

// RGB→HSV inverse.  Components in [0, 1]; returned hue in [0, 360).  Pure
// gray inputs (max == min) return hue = 0 by convention since there's no
// dominant channel — callers that jitter the result should not assume the
// returned hue is meaningful for desaturated colors.
inline Eigen::Vector3d RgbToHsv(double r, double g, double b) noexcept {
    const double mx = std::max({ r, g, b });
    const double mn = std::min({ r, g, b });
    const double delta = mx - mn;

    double h = 0.0;
    if (delta > 1e-9) {
        if (mx == r)
            h = 60.0 * std::fmod((g - b) / delta, 6.0);
        else if (mx == g)
            h = 60.0 * (((b - r) / delta) + 2.0);
        else
            h = 60.0 * (((r - g) / delta) + 4.0);
        if (h < 0.0) h += 360.0;
    }
    const double s = (mx <= 1e-9) ? 0.0 : (delta / mx);
    const double v = mx;
    return { h, s, v };
}

} // namespace wallpaper
