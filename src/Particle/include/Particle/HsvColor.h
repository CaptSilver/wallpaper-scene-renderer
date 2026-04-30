#pragma once

#include <Eigen/Core>
#include <algorithm>
#include <cmath>

namespace wallpaper
{

// Six-sextant HSV→RGB conversion.  `h` is in degrees [0, 360); `s` and `v`
// in [0, 1].  Out-of-range hue is wrapped via fmod; out-of-range s/v is
// clamped to [0, 1] so an author who jitters past the boundary doesn't
// produce negative or >1 RGB.
//
// Implementation note for mutation testing: the sextant dispatch uses an
// integer index (`floor(sextant) % 6`) instead of a cascading `< N` chain.
// At an exact sextant boundary (e.g. h=60 → sextant=1.0) the formula for
// `x` collapses both adjacent branches to the same RGB, which makes any
// `<` vs `<=` mutation on the cascade equivalent.  Switching on an integer
// index converts those boundary equivalents into `==` comparisons on
// distinct integer values that strict-mid-sextant fixtures can disambiguate.
inline Eigen::Vector3d HsvToRgb(double h, double s, double v) noexcept {
    // Normalize hue to [0, 360) via floored modulo (branch-free); the
    // alternative `if (h < 0.0) h += 360.0` has an equivalent mutation at
    // h=0 because both case 0 and case 5 produce the same RGB when x=0.
    h = h - std::floor(h / 360.0) * 360.0;
    s = std::clamp(s, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);

    const double c       = v * s;
    const double sextant = h / 60.0;
    const double x       = c * (1.0 - std::fabs(std::fmod(sextant, 2.0) - 1.0));
    const double m       = v - c;

    // Map to a 0..5 integer index.  Since h was normalized to [0, 360),
    // sextant is in [0, 6); floor lands in [0, 5].  The `default:` arm
    // catches the case=5 sextant and any rounding overshoot to 6.
    const int idx = static_cast<int>(std::floor(sextant));

    double r = 0, g = 0, b = 0;
    switch (idx) {
    case 0:  r = c; g = x; b = 0; break;
    case 1:  r = x; g = c; b = 0; break;
    case 2:  r = 0; g = c; b = x; break;
    case 3:  r = 0; g = x; b = c; break;
    case 4:  r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;  // case 5 (or rounding overshoot)
    }
    return { r + m, g + m, b + m };
}

// RGB→HSV inverse.  Components in [0, 1]; returned hue in [0, 360).  Pure
// gray inputs (max == min) return hue = 0 by convention since there's no
// dominant channel — callers that jitter the result should not assume the
// returned hue is meaningful for desaturated colors.
inline Eigen::Vector3d RgbToHsv(double r, double g, double b) noexcept {
    const double mx    = std::max({ r, g, b });
    const double mn    = std::min({ r, g, b });
    const double delta = mx - mn;

    double h = 0.0;
    // Strict `> 0.0` (rather than an epsilon `> 1e-9`) keeps the boundary
    // at delta==0, which is the only value where divide-by-zero could
    // strike.  An epsilon mutation `>= 1e-9` is mathematically equivalent
    // to `> 1e-9` for nearly all inputs — the only divergence is at
    // delta==1e-9, which is impossible to construct exactly with double
    // arithmetic from r/g/b in [0, 1].
    if (delta > 0.0) {
        if (mx == r)
            h = 60.0 * std::fmod((g - b) / delta, 6.0);
        else if (mx == g)
            h = 60.0 * (((b - r) / delta) + 2.0);
        else
            h = 60.0 * (((r - g) / delta) + 4.0);
        // Wrap negative hue (from r-channel branch where g<b) into [0, 360)
        // via floored modulo — branch-free so a `<` ↔ `<=` mutation has
        // no surface to survive on.  Without this, the `if (h<0)` boundary
        // at h==0 was equivalent because both branches produce the same
        // ([0,360) and 360) representation of red.
        h = h - std::floor(h / 360.0) * 360.0;
    }
    // Strict `> 0.0` for the saturation guard for the same reason as above.
    const double s = (mx > 0.0) ? (delta / mx) : 0.0;
    const double v = mx;
    return { h, s, v };
}

} // namespace wallpaper
