#include "WPParticleParser.hpp"
#include "Particle/ParticleEmitter.h"
#include "Particle/ParticleModify.h"
#include "Particle/ParticleSystem.h"
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
    };
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

ParticleInitOp WPParticleParser::genParticleInitOp(const nlohmann::json& wpj,
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
                PM::InitLifetime(p, Random::get(r.min, r.max));
            };
        } else if (name == "sizerandom") {
            SingleRandom r = { 0.0f, 20.0f };
            SingleRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                PM::InitSize(p, Random::get(r.min, r.max));
            };
        } else if (name == "alpharandom") {
            SingleRandom r = { 0.05f, 1.0f };
            SingleRandom::ReadFromJson(wpj, r);
            return [=](Particle& p, double) {
                PM::InitAlpha(p, Random::get(r.min, r.max));
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
                double r   = distance * 0.02 * std::cbrt(Random::get(0.0, 1.0));
                double z   = Random::get(-1.0, 1.0);
                double phi = Random::get(0.0, 2.0 * M_PI);
                double rxy = std::sqrt(1.0 - z * z);
                Vector3d offset(rxy * std::cos(phi), rxy * std::sin(phi), z);
                offset *= r;
                // Project out the along-line component so offset is perpendicular
                // to the control point line — produces clean zigzag for rope particles
                if (cp_size >= 2) {
                    Vector3d line = cp_data[1].offset - cp_data[0].offset;
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
                        val = (PM::GetPos(p).cast<double>() - cp_data[inputCP].offset).norm();
                    }
                }
                // Map val from [inMin, inMax] to [outMin, outMax]
                double t = (inMax > inMin) ? std::clamp((val - inMin) / (inMax - inMin), 0.0, 1.0)
                                           : 0.0;
                double outVal = algorism::lerp(t, (double)outMin, (double)outMax);

                if (output == "size") {
                    if (operation == "multiply")
                        PM::MutiplySize(p, outVal);
                    else
                        PM::InitSize(p, outVal);
                }
            };
        } else if (name == "mapsequencebetweencontrolpoints") {
            u32         count     = 10;
            u32         flags     = 0;
            float       arcamount = 0.0f;
            int         cpStart   = 0;
            std::string limitbehavior;
            GET_JSON_NAME_VALUE_NOWARN(wpj, "count", count);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "arcamount", arcamount);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "limitbehavior", limitbehavior);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "flags", flags);
            GET_JSON_NAME_VALUE_NOWARN(wpj, "controlpointstartindex", cpStart);
            if (count == 0) count = 1;
            cpStart = std::clamp(cpStart, 0, 7);

            // Mirror from explicit string or from flags bit 1
            bool                        mirror  = (limitbehavior == "mirror") || (flags & 2);
            const ParticleControlpoint* cp_data = controlpoints.data();
            usize                       cp_size = controlpoints.size();

            auto  seq_ptr = std::make_shared<u32>(0);
            // Noise phases for smooth curves — regenerated each emission cycle
            float phase0 = 0, phase1 = 0, phase2 = 0;
            return [=](Particle& p, double) mutable {
                u32& seq = *seq_ptr;
                if (cp_size < (usize)(cpStart + 2) || count <= 1) {
                    seq++;
                    return;
                }
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
                Vector3f cp0     = cp_data[cpStart].offset.cast<float>();
                Vector3f cp1     = cp_data[cpStart + 1].offset.cast<float>();
                Vector3f line    = cp1 - cp0;
                float    lineLen = line.norm();
                Vector3f pathpos = cp0 + t * line;

                // Perpendicular direction to the CP line
                Vector3f perp(-line.y(), line.x(), 0.0f);
                if (perp.squaredNorm() < 1e-6f)
                    perp = Vector3f(0, 0, 1.0f);
                else
                    perp.normalize();

                // Arc: parabolic bulge perpendicular to line
                if (std::abs(arcamount) > 1e-6f) {
                    pathpos += arcamount * 4.0f * t * (1.0f - t) * perp * lineLen;
                }

                // Smooth noise displacement: sum-of-sinusoids at 3 octaves
                // Creates organic curves instead of zigzag
                // Amplitude scales with line length (~15% of line length)
                float amplitude = lineLen * 0.15f;
                // Taper to zero at endpoints so bolt connects cleanly to CPs
                float taper     = std::sin(t * (float)M_PI);
                float noise     = std::sin(t * 2.5f * (float)M_PI + phase0) * 0.55f
                              + std::sin(t * 5.7f * (float)M_PI + phase1) * 0.30f
                              + std::sin(t * 11.3f * (float)M_PI + phase2) * 0.15f;
                pathpos += perp * noise * amplitude * taper;

                PM::Move(p, pathpos.cast<double>());
                PM::ChangeVelocity(p, -PM::GetVelocity(p).cast<double>());
                seq++;
            };
        }
    } while (false);
    return [](Particle&, double) {
    };
}

ParticleInitOp WPParticleParser::genOverrideInitOp(const wpscene::ParticleInstanceoverride& over) {
    return [=](Particle& p, double) {
        PM::MutiplyInitLifeTime(p, over.lifetime);
        PM::MutiplyInitAlpha(p, over.alpha);
        PM::MutiplyInitSize(p, over.size);
        PM::MutiplyVelocity(p, over.speed);
        if (over.overColor) {
            PM::InitColor(
                p, over.color[0] / 255.0f, over.color[1] / 255.0f, over.color[2] / 255.0f);
        } else if (over.overColorn) {
            PM::MutiplyInitColor(p, over.colorn[0], over.colorn[1], over.colorn[2]);
        }
        // Brightness multiplies particle color (e.g. 5.0 for lightning glow)
        if (over.brightness != 1.0f) {
            PM::MutiplyInitColor(p, over.brightness, over.brightness, over.brightness);
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
            Vector3d vecG   = Vector3f(gravity.data()).cast<double>();
            double   spd    = over.speed;
            return [=](const ParticleInfo& info) {
                for (auto& p : info.particles) {
                    Vector3d acc =
                        algorism::DragForce(PM::GetVelocity(p).cast<double>(), drag) + vecG * spd;
                    PM::Accelerate(p, acc, info.time_pass);
                    PM::MoveByTime(p, info.time_pass);
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
            auto vc        = ValueChange::ReadFromJson(wpj);
            auto size_over = over.size;
            return [vc, size_over](const ParticleInfo& info) {
                for (auto& p : info.particles)
                    PM::MutiplySize(p, size_over * FadeValueChange(PM::LifetimePos(p), vc));
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
                double noiseRate =
                    std::abs(tur.timescale * tur.scale * 2.0) * info.time_pass;
                bool incoherent = noiseRate > 1.0;

                for (auto& p : info.particles) {
                    Vector3d pos = PM::GetPos(p).cast<double>();
                    double factor = speed;
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
                Vector3d offset = info.controlpoints[v.controlpoint].offset +
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
        } else if (name == "maintaindistancebetweencontrolpoints") {
            // Clamp particles that drift past the control point endpoints.
            // The perpendicular offset (zigzag) from positionoffsetrandom/turbulence
            // is fully preserved; only the along-line component is clamped.
            return [=](const ParticleInfo& info) {
                if (info.controlpoints.size() < 2) return;
                Vector3d cp0  = info.controlpoints[0].offset;
                Vector3d cp1  = info.controlpoints[1].offset;
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
                Vector3d cpPos = info.controlpoints[controlpoint].offset;

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
                Vector3d offset = info.controlpoints[c.controlpoint].offset +
                                  Vector3f { c.origin.data() }.cast<double>();
                for (auto& p : info.particles) {
                    Vector3d diff     = offset - PM::GetPos(p).cast<double>();
                    double   distance = diff.norm();
                    if (distance < c.threshold) {
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
        box.minDistance    = wpe.distancemin;
        box.maxDistance    = wpe.distancemax;
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
        sphere.minDistance    = wpe.distancemin[0];
        sphere.maxDistance    = wpe.distancemax[0];
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
    if (! hasPeriodic) return baseOp;

    float minDelay = wpe.minperiodicdelay;
    float maxDelay = wpe.maxperiodicdelay;
    float minDur   = wpe.minperiodicduration;
    float maxDur   = wpe.maxperiodicduration;
    u32   maxPer   = wpe.maxtoemitperperiod;

    double phaseDur     = maxDur > 0 ? Random::get((double)minDur, (double)maxDur) : 0.1;
    // Random initial phase offset so different instances don't fire simultaneously
    double totalCycle   = (minDur + maxDur) * 0.5 + (minDelay + maxDelay) * 0.5;
    double timer        = totalCycle > 0 ? Random::get(0.0, totalCycle) : 0.0;
    bool   active       = true;
    u32    emittedCount = 0;

    return [=, baseOp = std::move(baseOp)](std::vector<Particle>&       ps,
                                           std::vector<ParticleInitOp>& inis, u32 maxcount,
                                           double timepass) mutable {
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

            // Trigger burst at start of period if maxtoemitperperiod is set.
            // Or for ropes, always try to emit the whole batch.
            double effectiveTime = timepass;
            if (justActivated && maxPer > 0) {
                effectiveTime = 1000.0; // Force immediate burst of maxPer
            } else if (batch_size > 1 && emittedCount == 0) {
                effectiveTime = 1000.0; // Force immediate rope batch
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
