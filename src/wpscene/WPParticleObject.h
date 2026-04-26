#pragma once
#include "WPJson.hpp"
#include "WPMaterial.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <array>
#include "Utils/BitFlags.hpp"
#include "Core/Literals.hpp"

namespace wallpaper
{
namespace fs
{
class VFS;
}
namespace wpscene
{

class ParticleControlpoint {
public:
    enum class FlagEnum
    {
        link_mouse = 0, // 1
        // this control point will follow the mouse cursor.
        worldspace = 1, // 2
        // the control point will always be at the same position in the world, independent from the
        // position of the particle system.
        follow_parent_particle = 2, // 4
        // (empirical — see memory/cp-parent-chain.md for the audit) this CP tracks the
        // spawner-parent's *live* particle position each frame.  Observed in NieR 2B
        // thunderbolt_child_spawner and dripping_water_droplets; always co-occurs with
        // parentcontrolpoint!=0, which provides the static fallback when no parent particle
        // is bound.
    };
    using EFlags = BitFlags<FlagEnum>;

    bool                 FromJson(const nlohmann::json&);
    EFlags               flags { 0 };
    i32                  id { -1 };
    std::array<float, 3> offset { 0, 0, 0 };
    // a static offset relative to the position of the particle system.
    bool offset_is_null { false };
    // True when JSON had `offset: null` (WE editor emits this for unassigned CP slots).
    // Distinct from an explicit `offset: "0 0 0"`.  Consumed by LoadControlPoint →
    // runtime `ParticleControlpoint::is_null_offset` for the bounded-pos fallback.
    i32 parentcontrolpoint { 0 };
    // Chain index into the parent subsystem's controlpoints array.  0 = no link (self); >0
    // pulls the resolved offset from parent->controlpoints[N] each frame.  Validated by
    // dripping_water_refract/splash (flags=0 chaining), droplets (flags=4 chaining), and
    // NieR 2B thunderbolt_child_spawner (flags=4 + chain fallback).
};

class ParticleRender {
public:
    bool        FromJson(const nlohmann::json&);
    std::string name;
    float       length { 0.05f };
    float       maxlength { 10.0f };
    float       subdivision { 3.0f };
};

class Initializer {
public:
    bool                 FromJson(const nlohmann::json&);
    std::array<float, 3> max { 0, 0, 0 };
    std::array<float, 3> min { 0, 0, 0 };
    std::string          name;
};

class Emitter {
public:
    enum class FlagEnum : uint32_t
    {
        one_per_frame = 1,
    };
    using EFlags = BitFlags<FlagEnum>;

public:
    bool                   FromJson(const nlohmann::json&);
    std::array<float, 3>   directions { 1.0f, 1.0f, 0.0f };
    std::array<float, 3>   distancemax { 256.0f, 256.0f, 256.0f };
    std::array<float, 3>   distancemin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>   origin { 0, 0, 0 };
    std::array<int32_t, 3> sign { 0, 0, 0 };
    u32                    instantaneous { 0 };
    float                  speedmin { 0 };
    float                  speedmax { 0 };
    u32                    audioprocessingmode { 0 };
    i32                    controlpoint { 0 };
    i32                    id;
    EFlags                 flags;
    std::string            name;
    float                  rate { 5.0f };

    // Periodic emission fields
    float minperiodicdelay { 0.0f };
    float maxperiodicdelay { 0.0f };
    float minperiodicduration { 0.0f };
    float maxperiodicduration { 0.0f };
    u32   maxtoemitperperiod { 0 };
    float duration { 0.0f }; // Emitter lifetime limit in seconds. 0 = unlimited.
};

class ParticleChild;
class Particle {
public:
    enum class FlagEnum
    {
        wordspace             = 0, // 1
        spritenoframeblending = 1, // 2
        perspective           = 2, // 4
    };
    using EFlags = BitFlags<FlagEnum>;

public:
    bool FromJson(const nlohmann::json&, fs::VFS&);

    std::vector<Emitter>              emitters;
    std::vector<nlohmann::json>       initializers;
    std::vector<nlohmann::json>       operators;
    std::vector<ParticleRender>       renderers;
    std::vector<ParticleControlpoint> controlpoints;

    WPMaterial material;

    std::vector<ParticleChild> children;

    std::string animationmode;
    float       sequencemultiplier { 1.0f };
    uint32_t    maxcount { 1 };
    float       starttime { 0.0f };
    EFlags      flags { 0 };
};
class ParticleChild {
public:
    bool FromJson(const nlohmann::json&, fs::VFS&);

    // static
    // eventfollow
    // eventspawn
    // eventdeath
    std::string type { "static" };
    std::string name;
    i32         maxcount { 20 };

    // flags
    i32   controlpointstartindex { 0 };
    float probability { 1.0f };

    std::array<float, 3> angles { 0, 0, 0 };
    std::array<float, 3> origin { 0, 0, 0 };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };

    Particle obj;
};

class ParticleInstanceoverride {
public:
    bool FromJosn(const nlohmann::json&);
    bool enabled { false };
    bool overColor { false };
    bool overColorn { false };

    float                alpha { 1.0f };
    float                brightness { 1.0f };
    float                count { 1.0f };
    // `lifetime` is the only field whose default differs from the others'
    // multiplicative identity (1.0).  The override dispatches through
    // `ApplyLifetimeOverride`, which on sprite/halo subsystems treats the
    // value as an ABSOLUTE per-particle duration (`SetInitLifeTime`) — not a
    // multiplier.  Default 1.0 would unconditionally stomp every authored
    // `lifetimerandom` range to "1 second" whenever any wallpaper authored
    // any instanceoverride block (because `enabled=true` flips on the entire
    // override-init function), even for fields the author didn't touch.
    // Default 0.0 routes through the `<= 0` short-circuit in
    // `ApplyLifetimeOverride` so absent-from-JSON means "no override".
    float                lifetime { 0.0f };
    float                rate { 1.0f };
    float                speed { 1.0f };
    float                size { 1.0f };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> colorn { 1.0f, 1.0f, 1.0f };

    struct ControlPointOverride {
        bool                 active { false };
        std::array<float, 3> offset { 0, 0, 0 };
        // Per-CP orientation override (radians, XYZ).  WE's editor exposes "Lock control
        // point angles" (see wpdoc/ui_en-us.json), so CPs are a frame (position + rotation)
        // at the authoring level even though the runtime CP struct has historically tracked
        // only position.  NieR 2B's lightning/discharge (obj 113, 716, 722) sets
        // `controlpointangle1` on CP[1]; no operator we've implemented currently consumes
        // the angle, but we plumb it through so a consumer can read it without a parse-layer
        // change, and so the data isn't silently dropped.
        std::array<float, 3> angles { 0, 0, 0 };
        bool                 anglesActive { false };
    };
    std::array<ControlPointOverride, 8> controlpointOverrides;
};

class WPParticleObject {
public:
    bool                     FromJson(const nlohmann::json&, fs::VFS&);
    int32_t                  id;
    int32_t                  parent_id { -1 };
    std::string              name;
    std::array<float, 3>     origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>     scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3>     angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2>     parallaxDepth { 0.0f, 0.0f };
    bool                     visible { true };
    // When true, the object does NOT inherit transform/visibility/alpha/tint
    // from parent groups or nodes.  WE editor flag; no groups exist in the
    // scenes we've encountered so the effect is currently nil, but the field
    // is parsed for completeness and future hierarchy support.
    bool                     disablepropagation { false };
    std::string              particle;
    Particle                 particleObj;
    ParticleInstanceoverride instanceoverride;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Initializer, name, max, min);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Emitter, name, distancemax, distancemin, rate, directions);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Particle, initializers, operators, emitters);
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WPParticleObject, name, origin, angles, scale, visible, particle,
                                   particleObj);
} // namespace wpscene
} // namespace wallpaper
