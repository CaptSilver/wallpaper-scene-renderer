#pragma once

#include "Particle/Particle.h"
#include "Particle/ParticleModify.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "WPJson.hpp"

namespace wallpaper
{

// Lifetime-blend window shared by every per-frame particle operator.  Authors
// can fade an operator's effect in/out across `LifetimePos(p) ∈ [0, 1]` via
// four optional JSON fields:
//
//   blendinstart   life value where fade-in begins    (default 0)
//   blendinend     life value where fade-in completes (default 0)  — when end
//                                                                    equals
//                                                                    start, no
//                                                                    fade-in
//                                                                    is applied
//   blendoutstart  life value where fade-out begins   (default 1)
//   blendoutend    life value where fade-out completes (default 1) — when end
//                                                                    equals
//                                                                    start, no
//                                                                    fade-out
//                                                                    is applied
//
// Each frame the operator's effect is multiplied by
//   factor(life) = fade_in(life) * fade_out(life)
//   fade_in(life)  = clamp((life - bistart) / (biend  - bistart),  0, 1)  if biend > bistart, else 1
//   fade_out(life) = 1 - clamp((life - bostart) / (boend - bostart), 0, 1) if boend > bostart, else 1
//
// When neither window is real, `Has()` returns false and `Factor()` always
// returns 1.0 — call sites can short-circuit the wrapping multiply entirely.
struct BlendWindow {
    float blendin_start { 0.0f };
    float blendin_end { 0.0f };
    float blendout_start { 1.0f };
    float blendout_end { 1.0f };
    bool  has_fade_in { false };
    bool  has_fade_out { false };

    static BlendWindow FromJson(const nlohmann::json& j) {
        BlendWindow bw;
        GET_JSON_NAME_VALUE_NOWARN(j, "blendinstart", bw.blendin_start);
        GET_JSON_NAME_VALUE_NOWARN(j, "blendinend", bw.blendin_end);
        GET_JSON_NAME_VALUE_NOWARN(j, "blendoutstart", bw.blendout_start);
        GET_JSON_NAME_VALUE_NOWARN(j, "blendoutend", bw.blendout_end);
        bw.has_fade_in  = bw.blendin_end > bw.blendin_start + 1e-6f;
        bw.has_fade_out = bw.blendout_end > bw.blendout_start + 1e-6f &&
                          bw.blendout_start < 1.0f - 1e-6f;
        return bw;
    }

    // Whether the blend window is non-trivial (actually scales the effect).
    bool Has() const noexcept { return has_fade_in || has_fade_out; }

    // Compute the blend factor for a particle's current lifetime position.
    // Returns 1.0 when neither fade-in nor fade-out is real.
    double Factor(const Particle& p) const noexcept {
        if (! Has()) return 1.0;
        return Factor(ParticleModify::LifetimePos(p));
    }

    double Factor(double life) const noexcept {
        if (! Has()) return 1.0;
        double f_in = 1.0;
        if (has_fade_in) {
            f_in = std::clamp(
                (life - blendin_start) / (double)(blendin_end - blendin_start), 0.0, 1.0);
        }
        double f_out = 1.0;
        if (has_fade_out) {
            const double t = std::clamp(
                (life - blendout_start) / (double)(blendout_end - blendout_start), 0.0, 1.0);
            f_out = 1.0 - t;
        }
        return f_in * f_out;
    }
};

} // namespace wallpaper
