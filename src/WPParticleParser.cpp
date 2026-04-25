#include "WPParticleParser.hpp"
#include "WPMapSequenceParse.hpp"
#include "Particle/ParticleEmitter.h"
#include "Particle/ParticleModify.h"
#include "Particle/ParticleSystem.h"
#include <chrono>
#include <random>
#include <memory>
#include <algorithm>
#include <cmath>

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include "Utils/Logging.h"
#include "Utils/Algorism.h"
#include "Core/Random.hpp"

using namespace wallpaper;
using namespace Eigen;
namespace PM = ParticleModify;

namespace
{

inline void Color(Particle& p, const std::array<float, 3> min, const std::array<float, 3> max) {
    double               random = Random::get(0.0, 1.0);
    std::array<float, 3> result;
    for (int32_t i = 0; i < 3; i++) {
        result[i] = (float)algorism::lerp(random, min[i], max[i]);
    }
    PM::InitColor(p, result[0], result[1], result[2]);
}

inline Vector3d GenRandomVec3(const std::array<float, 3>& min, const std::array<float, 3>& max) {
    Vector3d result(3);
    for (int32_t i = 0; i < 3; i++) {
        result[i] = Random::get(min[i], max[i]);
    }
    return result;
}

} // namespace

struct SingleRandom {
    float       min { 0.0f };
    float       max { 0.0f };
    float       exponent { 1.0f };
    static void ReadFromJson(const nlohmann::json& j, SingleRandom& r) {
        GET_JSON_NAME_VALUE_NOWARN(j, "min", r.min);
        GET_JSON_NAME_VALUE_NOWARN(j, "max", r.max);
        GET_JSON_NAME_VALUE_NOWARN(j, "exponent", r.exponent);
    };
    // Apply exponent to a uniform [0,1] random value: pow(rand, exponent) skews toward min
    float get() const {
        float t = Random::get(0.0f, 1.0f);
        if (exponent != 1.0f) t = std::pow(t, exponent);
        return min + (max - min) * t;
    }
};
struct VecRandom {
    std::array<float, 3> min { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> max { 0.0f, 0.0f, 0.0f };
    float                exponent { 1.0f };

    static void ReadFromJson(const nlohmann::json& j, VecRandom& r) {
        GET_JSON_NAME_VALUE_NOWARN(j, "min", r.min);
        GET_JSON_NAME_VALUE_NOWARN(j, "max", r.max);
    };
};
struct TurbulentRandom {
    float  scale { 1.0f };
    double timescale { 1.0f };
    float  offset { 0.0f };
    float  speedmin { 100.0f };
    float  speedmax { 250.0f };
    float  phasemin { 0.0f };
    float  phasemax { 0.1f };

    std::array<float, 3> forward { 0.0f, 1.0f, 0.0f }; // x y z
    std::array<float, 3> right { 0.0f, 0.0f, 1.0f };
    std::array<float, 3> up { 1.0f, 0.0f, 0.0f };

    static void ReadFromJson(const nlohmann::json& j, TurbulentRandom& r) {
        GET_JSON_NAME_VALUE_NOWARN(j, "scale", r.scale);
        GET_JSON_NAME_VALUE_NOWARN(j, "timescale", r.timescale);
        GET_JSON_NAME_VALUE_NOWARN(j, "offset", r.offset);
        GET_JSON_NAME_VALUE_NOWARN(j, "speedmin", r.speedmin);
        GET_JSON_NAME_VALUE_NOWARN(j, "speedmax", r.speedmax);
        GET_JSON_NAME_VALUE_NOWARN(j, "phasemin", r.phasemin);
        GET_JSON_NAME_VALUE_NOWARN(j, "phasemax", r.phasemax);
        GET_JSON_NAME_VALUE_NOWARN(j, "forward", r.forward);
        GET_JSON_NAME_VALUE_NOWARN(j, "right", r.right);
        GET_JSON_NAME_VALUE_NOWARN(j, "up", r.up);
    };
};
template<std::size_t N>
std::array<float, N> mapVertex(const std::array<float, N>& v, float (*oper)(float)) {
    std::array<float, N> result;
    std::transform(v.begin(), v.end(), result.begin(), oper);
    return result;
};

ParticleInitOp
WPParticleParser::genParticleInitOp(const nlohmann::json&                 wpj,
                                    std::span<const ParticleControlpoint> controlpoints) {
    using namespace std::placeholders;
    do {
        if (! wpj.contains("name")) break;
        std::string name;
        GET_JSON_NAME_VALUE(wpj, "name", name);

        if (name == "colorrandom") {
            VecRandom r;
            r.min = { 0.0f, 0.0f, 0.0f };
            r.max = { 255.0f, 255.0f, 255.0f };
            VecRandom::ReadFromJson(wpj, r);
            if (! wpj.contains("max")) r.max = r.min;

            return [=](Particle& p, double) {
                Color(p,
                      mapVertex(r.min,
                                [](float x) {
                                    return x / 255.0f;
                                }),
                      mapVertex(r.max, [](float x) {
                          return x / 255.0f;
                      }));
            };
        } else if (name == "lifetimerandom") {
            SingleRandom r = { 0.0f, 1.0f };
            SingleRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                float t = r.get();
                // When the wallpaper fixes min==max (common for star-field / warp
                // presets, e.g. Voyager starfield min=1 max=1), every particle in
                // a batch dies on exactly the same frame, producing a visible
                // death-pulse.  Add ±5% jitter so deaths stagger across several
                // frames naturally.  No-op when min != max already provides jitter.
                if (r.min == r.max && t > 0.0f) {
                    t *= 1.0f + Random::get(-0.05f, 0.05f);
                }
                PM::InitLifetime(p, t);
            };
        } else if (name == "sizerandom") {
            SingleRandom r = { 0.0f, 20.0f };
            SingleRandom::ReadFromJson(wpj, r);
            if (r.exponent != 1.0f) {
                LOG_INFO("sizerandom: min=%.2f max=%.2f exponent=%.2f", r.min, r.max, r.exponent);
            }
            return [=](Particle& p, double) {
                PM::InitSize(p, r.get());
            };
        } else if (name == "alpharandom") {
            SingleRandom r = { 0.05f, 1.0f };
            SingleRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                PM::InitAlpha(p, r.get());
            };
        } else if (name == "velocityrandom") {
            VecRandom r;
            r.min[0] = r.min[1] = -32.0f;
            r.max[0] = r.max[1] = 32.0f;
            VecRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                auto result = GenRandomVec3(r.min, r.max);
                PM::ChangeVelocity(p, result[0], result[1], result[2]);
            };
        } else if (name == "rotationrandom") {
            VecRandom r;
            r.max[2] = 2 * M_PI;
            VecRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                auto result = GenRandomVec3(r.min, r.max);
                PM::ChangeRotation(p, result[0], result[1], result[2]);
            };
        } else if (name == "angularvelocityrandom") {
            VecRandom r;
            r.min[2] = -5.0f;
            r.max[2] = 5.0f;
            VecRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                auto result = GenRandomVec3(r.min, r.max);
                PM::ChangeAngularVelocity(p, result[0], result[1], result[2]);
            };
        } else if (name == "turbulentvelocityrandom") {
            // to do
            TurbulentRandom r;
            TurbulentRandom::ReadFromJson(wpj, r);
            Vector3f forward(r.forward.data());
            Vector3f right(r.right.data());
            Vector3f pos = GenRandomVec3({ 0, 0, 0 }, { 10.0f, 10.0f, 10.0f }).cast<float>();
            return [=](Particle& p, double duration) mutable {
                float speed = Random::get(r.speedmin, r.speedmax);
                if (duration > 10.0f) {
                    pos[0] += speed;
                    duration = 0.0f;
                }
                Vector3f result;
                do {
                    result = algorism::CurlNoise(pos.cast<double>()).cast<float>().normalized();
                    pos += result * 0.005f / r.timescale;
                    duration -= 0.01f;
                } while (duration > 0.01f);
                // limit direction
                {
                    double c     = result.dot(forward) / (result.norm() * forward.norm());
                    float  a     = std::acos(c) / M_PI;
                    float  scale = r.scale / 2.0f;
                    if (a > scale) {
                        auto axis = result.cross(forward).normalized();
                        result    = AngleAxisf((a - a * scale) * M_PI, axis) * result;
                    }
                }
                // offset
                result = AngleAxisf(r.offset, right) * result;
                result *= speed;
                PM::ChangeVelocity(p, result[0], result[1], result[2]);
            };
        } else if (name == "positionoffsetrandom") {
            float distance = 0.0f;
            GET_JSON_NAME_VALUE_NOWARN(wpj, "distance", distance);
            const ParticleControlpoint* cp_data = controlpoints.data();
            usize                       cp_size = controlpoints.size();
            return [=](Particle& p, double) {
                if (distance <= 0.0f) return;
                // Small micro-variation on top of smooth noise curves from mapsequence
                double   r   = distance * 0.02 * std::cbrt(Random::get(0.0, 1.0));
                double   z   = Random::get(-1.0, 1.0);
                double   phi = Random::get(0.0, 2.0 * M_PI);
                double   rxy = std::sqrt(1.0 - z * z);
                Vector3d offset(rxy * std::cos(phi), rxy * std::sin(phi), z);
                offset *= r;
                // Project out the along-line component so offset is perpendicular
                // to the control point line — produces clean zigzag for rope particles
                if (cp_size >= 2) {
                    Vector3d line = cp_data[1].resolved - cp_data[0].resolved;
                    double   len2 = line.squaredNorm();
                    if (len2 > 1e-10) {
                        Vector3d dir = line / std::sqrt(len2);
                        offset -= offset.dot(dir) * dir;
                    }
                }
                PM::Move(p, offset);
            };
        } else if (name == "box") {
            VecRandom r;
            VecRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                auto pos = GenRandomVec3(r.min, r.max);
                PM::InitPos(p, pos[0], pos[1], pos[2]);
            };
        } else if (name == "sphere") {
            SingleRandom r;
            SingleRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                double radius =
                    algorism::lerp(std::pow(Random::get(0.0, 1.0), 1.0 / 3.0), r.min, r.max);
                double   z   = Random::get(-1.0, 1.0);
                double   phi = Random::get(0.0, 2.0 * M_PI);
                double   rxy = std::sqrt(1.0 - z * z);
                Vector3d pos(rxy * std::cos(phi), rxy * std::sin(phi), z);
                pos *= radius;
                PM::InitPos(p, pos[0], pos[1], pos[2]);
            };
        } else if (name == "remapinitialvalue") {
            std::string input, output, operation;
            float       inMin = 0, inMax = 100, outMin = 0, outMax = 1;
            int         inputCP = 0;
            GET_JSON_NAME_VALUE_NOWARN(wpj, "input", input);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "output", output);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "operation", operation);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "inputrangemin", inMin);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "inputrangemax", inMax);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "outputrangemin", outMin);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "outputrangemax", outMax);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "inputcontrolpoint0", inputCP);
            inputCP = std::clamp(inputCP, 0, 7);

            const ParticleControlpoint* cp_data = controlpoints.data();
            usize                       cp_size = controlpoints.size();

            return [=](Particle& p, double) {
                double val = 0;
                if (input == "distancetocontrolpoint") {
                    if ((usize)inputCP < cp_size) {
                        val = (PM::GetPos(p).cast<double>() - cp_data[inputCP].resolved).norm();
                    }
                }
                // Map val from [inMin, inMax] to [outMin, outMax]
                double t =
                    (inMax > inMin) ? std::clamp((val - inMin) / (inMax - inMin), 0.0, 1.0) : 0.0;
                double outVal = algorism::lerp(t, (double)outMin, (double)outMax);

                if (output == "size") {
                    if (operation == "multiply")
                        PM::MutiplySize(p, outVal);
                    else
                        PM::InitSize(p, outVal);
                }
            };
        } else if (name == "mapsequencebetweencontrolpoints") {
            // Field semantics (clamps, defaults, mirror toggle, arc / size-reduction
            // capture) are pinned in src/WPMapSequenceParse.hpp and exercised by
            // test_WPMapSequenceParse.cpp.  The captured params struct is what the
            // closure consumes — keeping the parse separate from the per-spawn maths
            // avoids re-reading the JSON on each emission and lets the test suite hit
            // the parse branch without owning a particle system.
            const auto                  params  = ParseBetweenParams(wpj);
            const u32                   count   = params.count;
            const float                 arcamount = params.arc_amount;
            const float                 size_reduction = params.size_reduction_amount;
            const std::array<float, 3>  arc_dir_arr = params.arc_direction;
            const int                   cpStart = params.cp_start;
            const int                   cpEnd   = params.cp_end;
            const bool                  mirror  = params.mirror;
            const ParticleControlpoint* cp_data = controlpoints.data();
            usize                       cp_size = controlpoints.size();

            auto seq_ptr = std::make_shared<u32>(0);
            // Wall-clock gap detection for periodic emitters.  When the parent subsystem has
            // `maxperiodicdelay > 0` (bursty emission with pauses, e.g. NieR 2B thunderbolt
            // at 1s-on/1s-off), the continuous seq counter doesn't reset between bursts.
            // With mirror=true and count=32, period=62, and batch=32/burst: after the first
            // burst (seq 0..31), the next burst re-enters at seq=32, which maps to idx=30
            // (mid-bolt).  The bolt then appears to "start in the middle".  Detecting a gap
            // in spawn times and resetting seq to 0 makes every burst draw a fresh bolt from
            // CP0 to CP1.
            //
            // Threshold 0.25s is comfortably above the per-particle interval for any rope
            // rate we've seen in the wild (rate=100 → 10ms, rate=10 → 100ms) and below the
            // shortest periodic delay observed (NieR: 1s).
            auto last_spawn_ptr = std::make_shared<std::chrono::steady_clock::time_point>();
            auto has_spawned_ptr = std::make_shared<bool>(false);
            constexpr double kGapResetSec = 0.25;
            // Noise phases for smooth curves — regenerated each emission cycle
            float phase0 = 0, phase1 = 0, phase2 = 0;
            return [=](Particle& p, double) mutable {
                u32& seq = *seq_ptr;
                // Both endpoints must lie inside the CP table.  cp_end may be lower,
                // higher, or equal to cp_start — the engine permits any pair.
                if (cp_size <= (usize)cpStart || cp_size <= (usize)cpEnd || count <= 1) {
                    seq++;
                    return;
                }
                // Detect burst-boundary gap and reset seq.  First spawn just records the
                // timestamp without resetting (seq is already 0).
                auto now = std::chrono::steady_clock::now();
                if (*has_spawned_ptr) {
                    double delta =
                        std::chrono::duration<double>(now - *last_spawn_ptr).count();
                    if (delta > kGapResetSec) {
                        seq = 0;
                    }
                }
                *last_spawn_ptr   = now;
                *has_spawned_ptr  = true;
                u32 idx;
                if (mirror && count > 1) {
                    u32 period = 2 * (count - 1);
                    u32 pos    = seq % period;
                    idx        = pos < count ? pos : period - pos;
                } else {
                    idx = seq % count;
                }
                // New cycle: randomize noise phases so each bolt looks different
                if (idx == 0) {
                    phase0 = Random::get(0.0f, (float)(2.0 * M_PI));
                    phase1 = Random::get(0.0f, (float)(2.0 * M_PI));
                    phase2 = Random::get(0.0f, (float)(2.0 * M_PI));
                }
                float    t       = (float)idx / (float)(count - 1);
                Vector3f cp0     = cp_data[cpStart].resolved.cast<float>();
                Vector3f cp1     = cp_data[cpEnd].resolved.cast<float>();
                Vector3f line    = cp1 - cp0;
                float    lineLen = line.norm();
                Vector3f pathpos = cp0 + t * line;

                // Arc bulge direction.  When the author supplied `arcdirection`
                // (non-zero vec3) the bow follows that vector; otherwise fall
                // back to the screen-plane perpendicular of the CP line so a
                // 2D arrangement still bulges sideways instead of collapsing.
                Vector3f arc_dir(arc_dir_arr[0], arc_dir_arr[1], arc_dir_arr[2]);
                if (arc_dir.squaredNorm() < 1e-6f) {
                    arc_dir = Vector3f(-line.y(), line.x(), 0.0f);
                    if (arc_dir.squaredNorm() < 1e-6f)
                        arc_dir = Vector3f(0, 0, 1.0f);
                }
                arc_dir.normalize();

                // Arc: parabolic bulge along arc_dir, scaled by chord length so
                // the visual amplitude tracks the rope's own size.
                if (std::abs(arcamount) > 1e-6f) {
                    pathpos += arcamount * 4.0f * t * (1.0f - t) * arc_dir * lineLen;
                }

                // Per-particle size reduction along the rope: when the author
                // sets `sizereductionamount` the leading particles stay full
                // size and the trailing particles taper down to (1 - amount)
                // of their original size.  Authored value 0 = no reduction
                // (matches today's behavior).
                if (size_reduction > 1e-6f) {
                    float scale = 1.0f - size_reduction * t;
                    if (scale < 0.0f) scale = 0.0f;
                    PM::MutiplySize(p, static_cast<double>(scale));
                }

                // Smooth noise displacement: sum-of-sinusoids at 3 octaves
                // Creates organic curves instead of zigzag.  Noise rides on
                // the screen-plane perpendicular (independent of arc_dir) so
                // the jitter direction stays stable even when arc_dir aligns
                // with the chord.
                Vector3f noise_perp(-line.y(), line.x(), 0.0f);
                if (noise_perp.squaredNorm() < 1e-6f)
                    noise_perp = Vector3f(0, 0, 1.0f);
                else
                    noise_perp.normalize();
                float amplitude = lineLen * 0.15f;
                float taper     = std::sin(t * (float)M_PI);
                float noise     = std::sin(t * 2.5f * (float)M_PI + phase0) * 0.55f +
                              std::sin(t * 5.7f * (float)M_PI + phase1) * 0.30f +
                              std::sin(t * 11.3f * (float)M_PI + phase2) * 0.15f;
                pathpos += noise_perp * noise * amplitude * taper;

                PM::Move(p, pathpos.cast<double>());
                // Explicitly zero velocity — particles are placed, not moving
                PM::InitVelocity(p, 0.0, 0.0, 0.0);
                seq++;
            };
        } else if (name == "mapsequencearoundcontrolpoint") {
            const auto                  params = ParseAroundParams(wpj);
            const u32                   count  = params.count;
            const int                   cp_id  = params.cp;
            const std::array<float, 3>  axis   = params.axis;

            LOG_INFO("mapsequencearoundcontrolpoint: count=%u axis=(%.1f,%.1f,%.1f) cp=%d",
                     count,
                     axis[0],
                     axis[1],
                     axis[2],
                     cp_id);

            const ParticleControlpoint* cp_data = controlpoints.data();
            usize                       cp_size = controlpoints.size();
            auto                        seq_ptr = std::make_shared<u32>(0);

            return [=](Particle& p, double) mutable {
                u32&  seq   = *seq_ptr;
                u32   idx   = seq % count;
                float angle = (float)idx / (float)count * 2.0f * (float)M_PI;

                Vector3f cp_offset(0, 0, 0);
                if ((usize)cp_id < cp_size) {
                    cp_offset = cp_data[cp_id].resolved.cast<float>();
                }

                Vector3f pos = cp_offset +
                               Vector3f(axis[0] * std::cos(angle), axis[1] * std::sin(angle), 0.0f);

                PM::Move(p, pos.cast<double>());
                // Explicitly zero velocity — particles are placed, not moving
                PM::InitVelocity(p, 0.0, 0.0, 0.0);
                seq++;
            };
        }
    } while (false);
    return [](Particle&, double) {
    };
}

ParticleInitOp WPParticleParser::genOverrideInitOp(const wpscene::ParticleInstanceoverride& over,
                                                   bool is_rope) {
    return [=](Particle& p, double) {
        // instanceoverride.lifetime semantic depends on renderer kind: sprites
        // and halos take it as absolute seconds (NieR 2B halo magic_vortex_1
        // ships lifetime=0.9 = "0.9s trails"); ropes take it as a multiplier
        // on the preset's randomized lifetime so per-segment timing scales
        // proportionally instead of collapsing to a flat constant.  See
        // ApplyLifetimeOverride for the dispatch and test_ParticleModify.cpp
        // for the pinned cases.
        if (over.enabled) {
            PM::ApplyLifetimeOverride(p, over.lifetime, is_rope);
        }
        PM::MutiplyInitAlpha(p, over.alpha);
        PM::MutiplyInitSize(p, over.size);
        PM::MutiplyVelocity(p, over.speed);
        PM::ApplyColorOverride(
            p, over.overColor, over.color, over.overColorn, over.colorn);
        // Brightness multiplies particle color (e.g. 5.0 for lightning glow).  For additive
        // blending with many overlapping sprites the raw `color * brightness` accumulates
        // aggressively in the HDR buffer — stacking 10 sprites with brightness=5 peaks at
        // ~5.0, which the FinPass tonemap `1 - exp(-hdr)` rounds to ~0.993 (near-white, hue
        // lost).  Compensate by reducing alpha by sqrt(brightness) so per-sprite
        // contribution scales with sqrt instead of linearly.  Preserves the artist's "this
        // should be brighter" intent (still boosts into HDR) but prevents the additive
        // stacking from saturating to white, retaining the blue tinge the colorn was
        // meant to convey.  Fixes NieR 2B thunderbolt_glow appearing as white blobs piled
        // at control points.
        if (over.brightness != 1.0f) {
            PM::MutiplyInitColor(p, over.brightness, over.brightness, over.brightness);
            if (over.brightness > 1.0f) {
                PM::MutiplyInitAlpha(p, 1.0f / std::sqrt(over.brightness));
            }
        }
    };
}
double FadeValueChange(float life, float start, float end, float startValue,
                       float endValue) noexcept {
    if (life <= start)
        return startValue;
    else if (life > end)
        return endValue;
    else {
        double pass = (life - start) / (end - start);
        return algorism::lerp(pass, startValue, endValue);
    }
}

struct ValueChange {
    float starttime { 0 };
    float endtime { 1.0f };
    float startvalue { 1.0f };
    float endvalue { 0.0f };

    static auto ReadFromJson(const nlohmann::json& j) {
        ValueChange v;
        GET_JSON_NAME_VALUE_NOWARN(j, "starttime", v.starttime);
        GET_JSON_NAME_VALUE_NOWARN(j, "endtime", v.endtime);
        GET_JSON_NAME_VALUE_NOWARN(j, "startvalue", v.startvalue);
        GET_JSON_NAME_VALUE_NOWARN(j, "endvalue", v.endvalue);
        return v;
    }
};
double FadeValueChange(float life, const ValueChange& v) noexcept {
    return FadeValueChange(life, v.starttime, v.endtime, v.startvalue, v.endvalue);
}

struct VecChange {
    float                starttime { 0 };
    float                endtime { 1.0f };
    std::array<float, 3> startvalue { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> endvalue { 0.0f, 0.0f, 0.0f };

    static auto ReadFromJson(const nlohmann::json& j) {
        VecChange v;
        GET_JSON_NAME_VALUE_NOWARN(j, "starttime", v.starttime);
        GET_JSON_NAME_VALUE_NOWARN(j, "endtime", v.endtime);
        GET_JSON_NAME_VALUE_NOWARN(j, "startvalue", v.startvalue);
        GET_JSON_NAME_VALUE_NOWARN(j, "endvalue", v.endvalue);
        return v;
    }
};

struct FrequencyValue {
    std::array<float, 3> mask { 1.0f, 1.0f, 0.0f };

    float frequencymin { 0.0f };
    float frequencymax { 10.0f };
    float scalemin { 0.0f };
    float scalemax { 1.0f };
    float phasemin { 0.0f };
    float phasemax { static_cast<float>(2 * M_PI) };

    struct StorageRandom {
        bool  reset { true };
        float frequency { 0.0f };
        float scale { 1.0f };
        float phase { 0.0f };
    };

    std::vector<StorageRandom> storage;

    static auto ReadFromJson(const nlohmann::json& j, std::string_view name) {
        FrequencyValue v;
        if (name == "oscillatesize") {
            v.scalemin = 0.8f;
            v.scalemax = 1.2f;
        } else if (name == "oscillateposition") {
            v.frequencymax = 5.0f;
        }
        GET_JSON_NAME_VALUE_NOWARN(j, "frequencymin", v.frequencymin);
        GET_JSON_NAME_VALUE_NOWARN(j, "frequencymax", v.frequencymax);
        if (v.frequencymax == 0.0f) v.frequencymax = v.frequencymin;
        GET_JSON_NAME_VALUE_NOWARN(j, "scalemin", v.scalemin);
        GET_JSON_NAME_VALUE_NOWARN(j, "scalemax", v.scalemax);
        GET_JSON_NAME_VALUE_NOWARN(j, "phasemin", v.phasemin);
        GET_JSON_NAME_VALUE_NOWARN(j, "phasemax", v.phasemax);
        GET_JSON_NAME_VALUE_NOWARN(j, "mask", v.mask);
        return v;
    };
    inline void CheckAndResize(size_t s) {
        if (storage.size() < s) storage.resize(2 * s, StorageRandom {});
    }
    inline void GenFrequency(Particle& p, uint32_t index) {
        auto& st = storage.at(index);
        if (! PM::LifetimeOk(p)) st.reset = true;
        if (st.reset) {
            st.frequency = Random::get(frequencymin, frequencymax);
            st.scale     = Random::get(scalemin, scalemax);
            st.phase     = (float)Random::get((double)phasemin, phasemax + 2.0 * M_PI);
            st.reset     = false;
        }
    }
    inline double GetScale(uint32_t index, double time) {
        const auto& st = storage.at(index);
        double      f  = st.frequency / (2.0f * M_PI);
        double      w  = 2.0f * M_PI * f;
        return algorism::lerp((std::cos(w * time + st.phase) + 1.0f) * 0.5f, scalemin, scalemax);
    }
    inline double GetMove(uint32_t index, double time, double timePass) {
        const auto& st = storage.at(index);
        double      f  = st.frequency / (2.0f * M_PI);
        double      w  = 2.0f * M_PI * f;
        return -1.0f * st.scale * w * std::sin(w * time + st.phase) * timePass;
    }
};

struct Turbulence {
    // the minimum time offset of the noise field for a particle.
    float phasemin { 0 };
    // the maximum time offset of the noise field for a particle.
    float phasemax { 0 };
    // the minimum velocity applied to particles.
    float speedmin { 500.0f };
    // the maximum velocity applied to particles.
    float speedmax { 1000.0f };
    // how fast the noise field changes shape.
    float timescale { 20.0f };

    float scale { 0.01f };

    std::array<int32_t, 3> mask { 1, 1, 0 };

    static auto ReadFromJson(const nlohmann::json& j) {
        Turbulence v;
        GET_JSON_NAME_VALUE_NOWARN(j, "phasemin", v.phasemin);
        GET_JSON_NAME_VALUE_NOWARN(j, "phasemax", v.phasemax);
        GET_JSON_NAME_VALUE_NOWARN(j, "speedmin", v.speedmin);
        GET_JSON_NAME_VALUE_NOWARN(j, "speedmax", v.speedmax);
        GET_JSON_NAME_VALUE_NOWARN(j, "timescale", v.timescale);
        GET_JSON_NAME_VALUE_NOWARN(j, "mask", v.mask);
        GET_JSON_NAME_VALUE_NOWARN(j, "scale", v.scale);
        return v;
    };
};

struct Vortex {
    enum class FlagEnum
    {
        infinit_axis = 0, // 1
    };
    using EFlags = BitFlags<FlagEnum>;

    i32 controlpoint { 0 };

    // anything below this distance receives force multiplied with speed inner.
    float distanceinner { 500.0f };
    // anything above this distance receives force multiplied with speed outer.
    float distanceouter { 650.0f };
    // amount of force applied to inner ring.
    float speedinner { 2500.0f };
    // amount of force applied to outer ring.
    float speedouter { 0 };

    EFlags flags { 0 };

    // positional offset from the center of the control point.
    std::array<float, 3> offset { 0.0f, 0.0f, 0.0f };

    // the axis to rotate around.
    std::array<float, 3> axis { 0.0f, 0.0f, 1.0f };

    static auto ReadFromJson(const nlohmann::json& j) {
        Vortex v;
        GET_JSON_NAME_VALUE_NOWARN(j, "controlpoint", v.controlpoint);
        if (v.controlpoint >= 8) LOG_ERROR("wrong contropoint index %d", v.controlpoint);
        v.controlpoint %= 8;

        GET_JSON_NAME_VALUE_NOWARN(j, "distanceinner", v.distanceinner);
        GET_JSON_NAME_VALUE_NOWARN(j, "distanceouter", v.distanceouter);
        GET_JSON_NAME_VALUE_NOWARN(j, "speedinner", v.speedinner);
        GET_JSON_NAME_VALUE_NOWARN(j, "speedouter", v.speedouter);

        i32 _flags { 0 };
        GET_JSON_NAME_VALUE_NOWARN(j, "flags", _flags);
        v.flags = EFlags(_flags);

        GET_JSON_NAME_VALUE_NOWARN(j, "offset", v.offset);
        GET_JSON_NAME_VALUE_NOWARN(j, "axis", v.axis);

        return v;
    };
};

struct ControlPointForce {
    i32 controlpoint { 0 };

    // how strongly the control point attracts or repels.
    float scale { 512.0f };
    // the maximum distance between particle and control point where the force takes effect.
    float threshold { 512.0f };

    // positional offset from the center of the control point.
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };

    static auto ReadFromJson(const nlohmann::json& j) {
        ControlPointForce v;
        GET_JSON_NAME_VALUE_NOWARN(j, "controlpoint", v.controlpoint);
        if (v.controlpoint >= 8) LOG_ERROR("wrong contropoint index %d", v.controlpoint);
        v.controlpoint %= 8;

        GET_JSON_NAME_VALUE_NOWARN(j, "scale", v.scale);
        GET_JSON_NAME_VALUE_NOWARN(j, "threadhold", v.threshold);
        GET_JSON_NAME_VALUE_NOWARN(j, "threshold", v.threshold);

        GET_JSON_NAME_VALUE_NOWARN(j, "offset", v.origin);
        return v;
    };
};

ParticleOperatorOp
WPParticleParser::genParticleOperatorOp(const nlohmann::json&                    wpj,
                                        const wpscene::ParticleInstanceoverride& over) {
    do {
        if (! wpj.contains("name")) break;
        std::string name;
        GET_JSON_NAME_VALUE(wpj, "name", name);
        if (name == "movement") {
            float drag { 0.0f };

            std::array<float, 3> gravity { 0, 0, 0 };
            GET_JSON_NAME_VALUE_NOWARN(wpj, "drag", drag);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "gravity", gravity);
            Vector3d vecG = Vector3f(gravity.data()).cast<double>();
            double   spd  = over.speed;
            return [=](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    // Gravity scaled by speed override
                    PM::Accelerate(p, vecG * spd, info.time_pass);
                    PM::MoveByTime(p, info.time_pass);
                    // Multiplicative drag: velocity *= (1 - drag * dt)
                    if (drag > 0.0f) {
                        double factor = std::max(0.0, 1.0 - drag * info.time_pass);
                        PM::MutiplyVelocity(p, factor);
                    }
                }
            };
        } else if (name == "angularmovement") {
            float                drag { 0.0f };
            std::array<float, 3> force { 0, 0, 0 };
            GET_JSON_NAME_VALUE_NOWARN(wpj, "drag", drag);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "force", force);
            Vector3d vecF = Vector3f(force.data()).cast<double>();
            return [=](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    Vector3d acc =
                        algorism::DragForce(PM::GetAngular(p).cast<double>(), drag) + vecF;
                    PM::AngularAccelerate(p, acc, info.time_pass);
                    PM::RotateByTime(p, info.time_pass);
                }
            };
        } else if (name == "sizechange") {
            auto vc = ValueChange::ReadFromJson(wpj);
            return [vc](const ParticleInfo& info) {
                for (auto& p : info.particles)
                    PM::MutiplySize(p, FadeValueChange(PM::LifetimePos(p), vc));
            };

        } else if (name == "alphafade") {
            float fadeintime { 0.5f }, fadeouttime { 0.5f };
            GET_JSON_NAME_VALUE_NOWARN(wpj, "fadeintime", fadeintime);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "fadeouttime", fadeouttime);
            return [fadeintime, fadeouttime](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    auto life = PM::LifetimePos(p);
                    if (life <= fadeintime)
                        PM::MutiplyAlpha(p, FadeValueChange(life, 0, fadeintime, 0, 1.0f));
                    else if (life > fadeouttime)
                        PM::MutiplyAlpha(p,
                                         1.0f - FadeValueChange(life, fadeouttime, 1.0f, 0, 1.0f));
                }
            };
        } else if (name == "alphachange") {
            auto vc = ValueChange::ReadFromJson(wpj);
            return [vc](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    PM::MutiplyAlpha(p, FadeValueChange(PM::LifetimePos(p), vc));
                }
            };
        } else if (name == "colorchange") {
            auto vc = VecChange::ReadFromJson(wpj);
            return [vc](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    auto     life = PM::LifetimePos(p);
                    Vector3f result;
                    for (uint i = 0; i < 3; i++)
                        result[i] = FadeValueChange(
                            life, vc.starttime, vc.endtime, vc.startvalue[i], vc.endvalue[i]);
                    PM::MutiplyColor(p, result[0], result[1], result[2]);
                }
            };
        } else if (name == "oscillatealpha") {
            FrequencyValue fv = FrequencyValue::ReadFromJson(wpj, name);
            return [fv](const ParticleInfo& info) mutable {
                fv.CheckAndResize(info.particles.size());
                for (uint i = 0; i < info.particles.size(); i++) {
                    auto& p = info.particles[i];
                    fv.GenFrequency(p, i);
                    PM::MutiplyAlpha(p, fv.GetScale(i, PM::LifetimePassed(p)));
                }
            };
        } else if (name == "oscillatesize") {
            FrequencyValue fv = FrequencyValue::ReadFromJson(wpj, name);
            return [fv](const ParticleInfo& info) mutable {
                fv.CheckAndResize(info.particles.size());
                for (uint i = 0; i < info.particles.size(); i++) {
                    auto& p = info.particles[i];
                    fv.GenFrequency(p, i);
                    PM::MutiplySize(p, fv.GetScale(i, PM::LifetimePassed(p)));
                }
            };

        } else if (name == "oscillateposition") {
            std::vector<Vector3f>         lastMove;
            FrequencyValue                fvx = FrequencyValue::ReadFromJson(wpj, name);
            std::array<FrequencyValue, 3> fxp = { fvx, fvx, fvx };
            return [=](const ParticleInfo& info) mutable {
                for (auto& f : fxp) f.CheckAndResize(info.particles.size());
                for (uint i = 0; i < info.particles.size(); i++) {
                    auto&    p = info.particles[i];
                    Vector3d del { Vector3d::Zero() };
                    auto     time = PM::LifetimePassed(p);
                    for (uint d = 0; d < 3; d++) {
                        if (fxp[0].mask[d] < 0.01) continue;
                        fxp[d].GenFrequency(p, i);
                        del[d] = fxp[d].GetMove(i, time, info.time_pass);
                    }

                    PM::Move(p, del);
                }
            };
        } else if (name == "turbulence") {
            Turbulence tur   = Turbulence::ReadFromJson(wpj);
            double     phase = Random::get(tur.phasemin, tur.phasemax);
            double     speed = Random::get(tur.speedmin, tur.speedmax);

            float blendinstart = 0.0f, blendinend = 0.0f;
            float blendoutstart = 1.0f, blendoutend = 1.0f;
            GET_JSON_NAME_VALUE_NOWARN(wpj, "blendinstart", blendinstart);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "blendinend", blendinend);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "blendoutstart", blendoutstart);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "blendoutend", blendoutend);
            bool hasBlend = blendinend > 0.0f || blendoutend < 1.0f;

            return [=](const ParticleInfo& info) {
                double noiseRate  = std::abs(tur.timescale * tur.scale * 2.0) * info.time_pass;
                bool   incoherent = noiseRate > 1.0;

                for (auto& p : info.particles) {
                    Vector3d pos    = PM::GetPos(p).cast<double>();
                    double   factor = speed;
                    if (hasBlend) {
                        double life  = PM::LifetimePos(p);
                        double blend = 1.0;
                        if (blendinend > blendinstart && life < blendinend)
                            blend = std::clamp(
                                (life - blendinstart) / (blendinend - blendinstart), 0.0, 1.0);
                        if (blendoutend > blendoutstart && life > blendoutstart) {
                            double bo = std::clamp(
                                (life - blendoutstart) / (blendoutend - blendoutstart), 0.0, 1.0);
                            blend *= (1.0 - bo);
                        }
                        factor *= blend;
                    }
                    if (incoherent) {
                        // High-frequency spatial noise so adjacent rope particles
                        // jitter independently (not uniformly).  Time evolves slowly
                        // so the crackle pattern drifts rather than flickering.
                        Vector3d samplePos = pos * tur.scale * 20.0;
                        samplePos.x() += phase + info.time * tur.scale * 2.0;
                        Vector3d result = algorism::CurlNoise(samplePos).normalized();
                        for (usize i = 0; i < 3; i++) {
                            if (tur.mask[i] == 0) result[i] = 0;
                        }
                        PM::Move(p, result * factor * info.time_pass * 0.3);
                    } else {
                        pos.x() += phase + tur.timescale * info.time;
                        Vector3d result =
                            speed * algorism::CurlNoise(pos * tur.scale * 2).normalized();
                        for (usize i = 0; i < 3; i++) {
                            if (tur.mask[i] == 0) result[i] = 0;
                        }
                        PM::Accelerate(p, result * factor, info.time_pass);
                    }
                }
            };
        } else if (name == "vortex") {
            Vortex v = Vortex::ReadFromJson(wpj);
            return [=](const ParticleInfo& info) {
                Vector3d offset = info.controlpoints[v.controlpoint].resolved +
                                  (Vector3f { v.offset.data() }).cast<double>();
                Vector3d axis    = (Vector3f { v.axis.data() }).cast<double>();
                double   dis_mid = v.distanceouter - v.distanceinner + 0.1f;

                for (auto& p : info.particles) {
                    Vector3d pos      = p.position.cast<double>();
                    Vector3d direct   = -axis.cross(pos).normalized();
                    double   distance = (pos - offset).norm();
                    if (dis_mid < 0 || distance < v.distanceinner) {
                        PM::Accelerate(p, direct * v.speedinner, info.time_pass);
                    }
                    if (distance > v.distanceouter) {
                        PM::Accelerate(p, direct * v.speedouter, info.time_pass);
                    } else if (distance > v.distanceinner) {
                        double t = (distance - v.distanceinner) / dis_mid;
                        PM::Accelerate(p,
                                       direct * algorism::lerp(t, v.speedinner, v.speedouter),
                                       info.time_pass);
                    }
                }
            };
        } else if (name == "vortex_v2") {
            Vortex v                = Vortex::ReadFromJson(wpj);
            float  ringradius       = 0.0f;
            float  ringwidth        = 0.0f;
            float  ringpulldistance = 0.0f;
            GET_JSON_NAME_VALUE_NOWARN(wpj, "ringradius", ringradius);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "ringwidth", ringwidth);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "ringpulldistance", ringpulldistance);
            bool use_rotation = v.flags[1]; // flags & 2: direct rotation mode
            LOG_INFO("particle operator vortex_v2: speedinner=%.1f speedouter=%.1f "
                     "distinner=%.1f distouter=%.1f ringradius=%.1f rotation=%d",
                     v.speedinner,
                     v.speedouter,
                     v.distanceinner,
                     v.distanceouter,
                     ringradius,
                     (int)use_rotation);
            return [=](const ParticleInfo& info) {
                Vector3d offset = info.controlpoints[v.controlpoint].resolved +
                                  (Vector3f { v.offset.data() }).cast<double>();
                Vector3d axis    = (Vector3f { v.axis.data() }).cast<double>().normalized();
                double   dis_mid = v.distanceouter - v.distanceinner + 0.1f;

                for (auto& p : info.particles) {
                    if (! PM::LifetimeOk(p)) continue;

                    Vector3d pos      = p.position.cast<double>();
                    Vector3d radial   = pos - offset;
                    double   distance = radial.norm();

                    // Determine speed based on distance zone
                    double speed;
                    if (dis_mid < 0 || distance < v.distanceinner) {
                        speed = v.speedinner;
                    } else if (distance > v.distanceouter) {
                        speed = v.speedouter;
                    } else {
                        double t = (distance - v.distanceinner) / dis_mid;
                        speed    = algorism::lerp(t, v.speedinner, v.speedouter);
                    }

                    if (use_rotation && distance > 0.001) {
                        // Direct rotation: rotate position around axis (Rodrigues').
                        // Preserves orbital radius while movement operator's drag
                        // naturally dampens the initial radial velocity from the emitter,
                        // giving a gentle outward drift for a softer ring appearance.
                        double   angle   = (speed / distance) * info.time_pass;
                        Vector3d rotated = radial * std::cos(angle) +
                                           axis.cross(radial) * std::sin(angle) +
                                           axis * axis.dot(radial) * (1.0 - std::cos(angle));
                        p.position = (offset + rotated).cast<float>();
                        // Zero velocity — rotation handles orbital motion directly,
                        // so the emitter's initial velocity is not needed.
                        p.velocity = Eigen::Vector3f::Zero();
                    } else {
                        // Acceleration mode (original vortex behavior)
                        Vector3d direct = axis.cross(radial).normalized();
                        PM::Accelerate(p, direct * speed, info.time_pass);
                    }

                    // Ring pull: attract particles toward target ring radius
                    if (ringradius > 0 && distance > 0.001) {
                        double   radiusDiff = ringradius - distance;
                        Vector3d radialDir  = radial / distance;
                        if (std::abs(radiusDiff) > ringwidth) {
                            double sign = radiusDiff > 0 ? 1.0 : -1.0;
                            PM::Accelerate(p, radialDir * ringpulldistance * sign, info.time_pass);
                        }
                    }
                }
            };
        } else if (name == "maintaindistancebetweencontrolpoints") {
            // Clamp particles that drift past the control point endpoints.
            // The perpendicular offset (zigzag) from positionoffsetrandom/turbulence
            // is fully preserved; only the along-line component is clamped.
            return [=](const ParticleInfo& info) {
                if (info.controlpoints.size() < 2) return;
                Vector3d cp0  = info.controlpoints[0].resolved;
                Vector3d cp1  = info.controlpoints[1].resolved;
                Vector3d line = cp1 - cp0;
                double   len2 = line.squaredNorm();
                if (len2 < 1e-10) return;

                for (auto& p : info.particles) {
                    if (! PM::LifetimeOk(p)) continue;
                    Vector3d pos   = PM::GetPos(p).cast<double>();
                    double   projT = (pos - cp0).dot(line) / len2;
                    if (projT < 0.0 || projT > 1.0) {
                        double   clampedT = std::clamp(projT, 0.0, 1.0);
                        Vector3d projPt   = cp0 + projT * line;
                        Vector3d clampPt  = cp0 + clampedT * line;
                        PM::Move(p, clampPt - projPt);
                    }
                }
            };
        } else if (name == "reducemovementnearcontrolpoint") {
            i32   controlpoint   = 0;
            float distanceinner  = 0.0f;
            float distanceouter  = 100.0f;
            float reductioninner = 1.0f;
            GET_JSON_NAME_VALUE_NOWARN(wpj, "controlpoint", controlpoint);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "distanceinner", distanceinner);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "distanceouter", distanceouter);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "reductioninner", reductioninner);
            controlpoint = std::clamp(controlpoint, 0, 7);

            return [=](const ParticleInfo& info) {
                if ((usize)controlpoint >= info.controlpoints.size()) return;
                Vector3d cpPos = info.controlpoints[controlpoint].resolved;

                for (auto& p : info.particles) {
                    if (! PM::LifetimeOk(p)) continue;
                    double dist = (PM::GetPos(p).cast<double>() - cpPos).norm();
                    if (dist >= distanceouter) continue;

                    double reduction;
                    if (dist <= distanceinner) {
                        reduction = reductioninner;
                    } else {
                        double t  = (dist - distanceinner) / (distanceouter - distanceinner);
                        reduction = algorism::lerp(t, (double)reductioninner, 0.0);
                    }
                    double factor = 1.0 / (1.0 + reduction * info.time_pass);
                    PM::MutiplyVelocity(p, factor);
                }
            };
        } else if (name == "remapvalue") {
            std::string input, output, transformfunction;
            float       transforminputscale = 1.0f;
            float       blendinstart = 0.0f, blendinend = 0.0f;
            float       blendoutstart = 1.0f, blendoutend = 1.0f;

            GET_JSON_NAME_VALUE_NOWARN(wpj, "input", input);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "output", output);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "transformfunction", transformfunction);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "transforminputscale", transforminputscale);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "blendinstart", blendinstart);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "blendinend", blendinend);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "blendoutstart", blendoutstart);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "blendoutend", blendoutend);

            return [=](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    if (! PM::LifetimeOk(p)) continue;

                    // Get input value
                    double inputVal = 0.0;
                    if (input == "particlesystemtime") {
                        inputVal = info.time;
                    } else if (input == "particlelifetime") {
                        inputVal = PM::LifetimePos(p);
                    }

                    // Apply transform function
                    double transformed = inputVal * transforminputscale;
                    if (transformfunction == "sine") {
                        transformed = (std::sin(transformed) + 1.0) * 0.5;
                    } else if (transformfunction == "cosine") {
                        transformed = (std::cos(transformed) + 1.0) * 0.5;
                    }
                    // else linear: keep as-is

                    // Blend based on particle lifetime position
                    double life  = PM::LifetimePos(p);
                    double blend = 1.0;
                    if (blendinend > blendinstart && life < blendinend) {
                        blend = std::clamp(
                            (life - blendinstart) / (blendinend - blendinstart), 0.0, 1.0);
                    }
                    if (blendoutend > blendoutstart && life > blendoutstart) {
                        double bout = std::clamp(
                            (life - blendoutstart) / (blendoutend - blendoutstart), 0.0, 1.0);
                        blend *= (1.0 - bout);
                    }

                    double value = algorism::lerp(blend, 1.0, transformed);

                    // Apply to output
                    if (output == "opacity") {
                        PM::MutiplyAlpha(p, value);
                    } else if (output == "size") {
                        PM::MutiplySize(p, value);
                    }
                }
            };
        } else if (name == "controlpointattract") {
            ControlPointForce c = ControlPointForce::ReadFromJson(wpj);
            return [=](const ParticleInfo& info) {
                Vector3d offset = info.controlpoints[c.controlpoint].resolved +
                                  Vector3f { c.origin.data() }.cast<double>();
                for (auto& p : info.particles) {
                    Vector3d diff     = offset - PM::GetPos(p).cast<double>();
                    double   distance = diff.norm();
                    if (distance < c.threshold) {
                        PM::Accelerate(p, diff.normalized() * c.scale, info.time_pass);
                    }
                }
            };
        } else if (name == "controlpointforce") {
            ControlPointForce c = ControlPointForce::ReadFromJson(wpj);
            return [=](const ParticleInfo& info) {
                Vector3d offset = info.controlpoints[c.controlpoint].resolved +
                                  Vector3f { c.origin.data() }.cast<double>();
                for (auto& p : info.particles) {
                    Vector3d diff     = PM::GetPos(p).cast<double>() - offset;
                    double   distance = diff.norm();
                    if (distance < c.threshold && distance > 0.0) {
                        PM::Accelerate(p, diff.normalized() * c.scale, info.time_pass);
                    }
                }
            };
        }
    } while (false);
    return [](const ParticleInfo&) {
    };
}

ParticleEmittOp WPParticleParser::genParticleEmittOp(const wpscene::Emitter& wpe, bool sort,
                                                     u32 batch_size, float burst_rate) {
    ParticleEmittOp baseOp;
    if (wpe.name == "boxrandom") {
        ParticleBoxEmitterArgs box;
        box.emitSpeed     = wpe.rate;
        box.minDistance   = wpe.distancemin;
        box.maxDistance   = wpe.distancemax;
        box.directions    = wpe.directions;
        box.orgin         = wpe.origin;
        box.one_per_frame = wpe.flags[wpscene::Emitter::FlagEnum::one_per_frame];
        box.instantaneous = wpe.instantaneous;
        box.minSpeed      = wpe.speedmin;
        box.maxSpeed      = wpe.speedmax;
        box.sort          = sort;
        box.batchSize     = batch_size;
        box.burstRate     = burst_rate;
        baseOp            = ParticleBoxEmitterArgs::MakeEmittOp(box);
    } else if (wpe.name == "sphererandom") {
        ParticleSphereEmitterArgs sphere;
        sphere.emitSpeed     = wpe.rate;
        sphere.minDistance   = wpe.distancemin[0];
        sphere.maxDistance   = wpe.distancemax[0];
        sphere.directions    = wpe.directions;
        sphere.orgin         = wpe.origin;
        sphere.sign          = wpe.sign;
        sphere.one_per_frame = wpe.flags[wpscene::Emitter::FlagEnum::one_per_frame];
        sphere.instantaneous = wpe.instantaneous;
        sphere.minSpeed      = wpe.speedmin;
        sphere.maxSpeed      = wpe.speedmax;
        sphere.sort          = sort;
        sphere.batchSize     = batch_size;
        sphere.burstRate     = burst_rate;
        baseOp               = ParticleSphereEmitterArgs::MakeEmittOp(sphere);
    } else {
        return [](std::vector<Particle>&, std::vector<ParticleInitOp>&, uint32_t, float) {
        };
    }

    // Wrap with periodic emission if configured
    bool hasPeriodic = wpe.maxperiodicduration > 0 || wpe.maxperiodicdelay > 0;
    if (hasPeriodic) {
        float minDelay = wpe.minperiodicdelay;
        float maxDelay = wpe.maxperiodicdelay;
        float minDur   = wpe.minperiodicduration;
        float maxDur   = wpe.maxperiodicduration;
        u32   maxPer   = wpe.maxtoemitperperiod;

        double phaseDur = maxDur > 0 ? Random::get((double)minDur, (double)maxDur) : 0.1;
        // Random initial phase offset so different instances don't fire simultaneously
        double totalCycle   = (minDur + maxDur) * 0.5 + (minDelay + maxDelay) * 0.5;
        double timer        = totalCycle > 0 ? Random::get(0.0, totalCycle) : 0.0;
        bool   active       = true;
        u32    emittedCount = 0;

        baseOp = [=, baseOp = std::move(baseOp)](std::vector<Particle>&       ps,
                                                 std::vector<ParticleInitOp>& inis,
                                                 u32                          maxcount,
                                                 double                       timepass) mutable {
            bool justActivated = false;
            timer += timepass;
            while (timer >= phaseDur) {
                timer -= phaseDur;
                active = ! active;
                if (active) {
                    phaseDur      = maxDur > 0 ? Random::get((double)minDur, (double)maxDur) : 0.1;
                    emittedCount  = 0;
                    justActivated = true;
                } else {
                    phaseDur = maxDelay > 0 ? Random::get((double)minDelay, (double)maxDelay) : 0.1;
                }
            }
            if (active && (maxPer == 0 || emittedCount < maxPer)) {
                u32 aliveBefore = 0;
                if (maxPer > 0) {
                    for (const auto& p : ps)
                        if (ParticleModify::LifetimeOk(p)) aliveBefore++;
                }

                // For non-rope periodic emitters with maxtoemitperperiod, burst at start.
                // For ropes: let them grow naturally (1 per frame via baseOp).
                double effectiveTime = timepass;
                if (justActivated && maxPer > 0 && batch_size <= 1) {
                    effectiveTime = 1000.0; // Force immediate burst of maxPer
                }

                baseOp(ps, inis, maxcount, effectiveTime);

                if (maxPer > 0) {
                    u32 aliveAfter = 0;
                    for (const auto& p : ps)
                        if (ParticleModify::LifetimeOk(p)) aliveAfter++;
                    if (aliveAfter > aliveBefore) emittedCount += (aliveAfter - aliveBefore);
                }
            }
        };
    }

    // Wrap with duration limiter if configured (outermost wrapper)
    float duration = wpe.duration;
    if (duration > 0.0f) {
        return
            [duration, elapsed = 0.0, baseOp = std::move(baseOp)](std::vector<Particle>&       ps,
                                                                  std::vector<ParticleInitOp>& inis,
                                                                  u32    maxcount,
                                                                  double timepass) mutable {
                elapsed += timepass;
                if (elapsed <= duration) {
                    baseOp(ps, inis, maxcount, timepass);
                }
            };
    }

    return baseOp;
}
