#pragma once

// Pure parser-side helpers for WE's particle subsystem CP / mapsequence /
// rope operators.  Header-only so doctest can exercise them without linking
// the runtime particle pipeline.
//
// Behavior is pinned by the parser-layer behavioral spec captured in
// memory/we-cp-rope-spec.md.  Implementation language is clean-room: we
// describe what the engine accepts and how it normalizes those values, not
// how the spec was produced.

#include "WPJson.hpp"
#include "Particle/ParticleEmitter.h"
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

namespace wallpaper
{

// `kCpReferencedFlag`, `kMaxCpIndex`, `ClampCpIndex` live in
// Particle/ParticleEmitter.h so the runtime doesn't need a JSON dep just to read
// the constant.  This header pulls them in transitively.

// `limitbehavior` is parsed as a string by the engine but only the literal
// "mirror" turns the cycle into back-and-forth.  Every other value
// (including the field's own default of "repeat" and any unknown / absent
// value) leaves the cycle in repeat mode.
inline bool LimitBehaviorIsMirror(std::string_view s) { return s == "mirror"; }

struct MapSequenceBetweenParams {
    uint32_t             count { 1 };
    int32_t              cp_start { 0 };
    int32_t              cp_end { 1 };
    bool                 mirror { false };
    uint32_t             flags { 0 };
    float                arc_amount { 0.0f };
    std::array<float, 3> arc_direction { 0.0f, 0.0f, 0.0f };
    float                size_reduction_amount { 0.0f };
};

struct MapSequenceAroundParams {
    uint32_t             count { 1 };
    int32_t              cp { 0 };
    bool                 mirror { false };
    uint32_t             flags { 0 };
    std::array<float, 3> axis { 0.0f, 0.0f, 0.0f };
};

inline MapSequenceBetweenParams ParseBetweenParams(const nlohmann::json& j) {
    MapSequenceBetweenParams p;
    uint32_t                 count = 1;
    GET_JSON_NAME_VALUE_NOWARN(j, "count", count);
    if (count == 0) count = 1;
    p.count = count;

    GET_JSON_NAME_VALUE_NOWARN(j, "flags", p.flags);

    int32_t cp_start = 0;
    int32_t cp_end   = 1;
    GET_JSON_NAME_VALUE_NOWARN(j, "controlpointstart", cp_start);
    GET_JSON_NAME_VALUE_NOWARN(j, "controlpointend", cp_end);
    p.cp_start = ClampCpIndex(cp_start);
    p.cp_end   = ClampCpIndex(cp_end);

    std::string limit;
    GET_JSON_NAME_VALUE_NOWARN(j, "limitbehavior", limit);
    // `flags` low bit 0x02 is treated as a mirror force in the wallpapers
    // we have hands-on data for (NieR thunderbolt presets ship limitbehavior
    // omitted but flags=23 = 0x10|0x04|0x02|0x01).  The authored bit
    // semantics aren't fully specified, so the OR keeps the effective
    // mirror toggle conservative: explicit string OR the empirical bit.
    p.mirror = LimitBehaviorIsMirror(limit) || (p.flags & 0x02u);

    GET_JSON_NAME_VALUE_NOWARN(j, "arcamount", p.arc_amount);
    GET_JSON_NAME_VALUE_NOWARN(j, "arcdirection", p.arc_direction);
    GET_JSON_NAME_VALUE_NOWARN(j, "sizereductionamount", p.size_reduction_amount);
    return p;
}

inline MapSequenceAroundParams ParseAroundParams(const nlohmann::json& j) {
    MapSequenceAroundParams p;
    uint32_t                count = 1;
    GET_JSON_NAME_VALUE_NOWARN(j, "count", count);
    if (count == 0) count = 1;
    p.count = count;

    GET_JSON_NAME_VALUE_NOWARN(j, "flags", p.flags);

    int32_t cp = 0;
    GET_JSON_NAME_VALUE_NOWARN(j, "controlpoint", cp);
    p.cp = ClampCpIndex(cp);

    std::string limit;
    GET_JSON_NAME_VALUE_NOWARN(j, "limitbehavior", limit);
    p.mirror = LimitBehaviorIsMirror(limit) || (p.flags & 0x02u);

    GET_JSON_NAME_VALUE_NOWARN(j, "axis", p.axis);
    return p;
}

// Walk an operator / initializer JSON block and return every CP slot index
// it names.  The parser uses this to OR `kCpReferencedFlag` into the
// runtime flags of every CP that any operator touches — independent of
// what the operator actually does with that CP at simulation time.
//
// Indices are clamped into [0..7] so a referenced-but-out-of-range slot
// still trips the bit on the highest valid slot, matching the engine's
// own clamp-on-read idiom.
inline std::vector<int32_t> CollectCpReferences(const nlohmann::json& op) {
    std::vector<int32_t> refs;
    auto                 push_if_present = [&](const char* key) {
        if (op.contains(key) && op.at(key).is_number_integer()) {
            int32_t v = op.at(key).get<int32_t>();
            refs.push_back(ClampCpIndex(v));
        }
    };
    // Singular form — controlpointforce / controlpointattract /
    // maintaindistancetocontrolpoint / reducemovementnearcontrolpoint /
    // mapsequencearoundcontrolpoint / inheritcontrolpointvelocity all use
    // the same field name.
    push_if_present("controlpoint");
    // Pair form used by the *_betweencontrolpoints operators.
    push_if_present("controlpointstart");
    push_if_present("controlpointend");
    // remapinitialvalue with input=distancetocontrolpoint reads input/output
    // CP pairs.  Output indices are accepted but not consumed by the
    // engine; we still record them so the referenced-flag mark is complete.
    push_if_present("inputcontrolpoint0");
    push_if_present("inputcontrolpoint1");
    push_if_present("outputcontrolpoint0");
    push_if_present("outputcontrolpoint1");
    return refs;
}

} // namespace wallpaper
