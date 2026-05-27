#pragma once

#include <vector>

namespace wallpaper
{

// Split a scene-time delta into <= cap-sized chunks so the per-frame
// simulation reads (CP velocity divisor, trail timestamps, g_Time, particle
// emission rate) all see the same scene.frameTime each step.  The trailing
// residual lands in [cap, cap * 1.5) so a 1.4x-cap frame becomes ONE step
// (avoiding compounding rounding from cap + 0.4*cap); a 1.6x-cap frame
// becomes two (32ms + 19.2ms).  Returns the ordered list of step sizes;
// summing the vector reproduces dt_scene exactly.
//
// Degenerate inputs (dt_scene <= 0, cap <= 0) return an empty plan so the
// caller skips simulation entirely.  Header-inline so the unit test reaches
// the pure math without pulling in Vulkan / SceneWallpaper deps.
inline std::vector<double> computeSubTickPlan(double dt_scene, double cap) {
    std::vector<double> plan;
    if (dt_scene <= 0.0 || cap <= 0.0) return plan;
    double remaining = dt_scene;
    while (remaining > cap * 1.5) {
        plan.push_back(cap);
        remaining -= cap;
    }
    plan.push_back(remaining);
    return plan;
}

} // namespace wallpaper
