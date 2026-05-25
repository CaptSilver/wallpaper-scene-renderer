#pragma once
#include "WPJson.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <vector>
#include "WPPuppet.hpp"

namespace wallpaper
{
namespace fs
{
class VFS;
}

namespace wpscene
{

class WPLightObject {
public:
    bool                 FromJson(const nlohmann::json&, fs::VFS&);
    int32_t              id { 0 };
    // JSON `parent` — scene-graph parent node id; -1 when authored at scene root.
    // Real-Time Earth (3557068717) parents its 2 point lights to the animated
    // SUN m5 node so the lights orbit with the computed sun position; without
    // honoring this field the lights collapse at world origin (Earth's center).
    int32_t              parent_id { -1 };
    std::string          name;
    std::array<float, 3> origin { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> scale { 1.0f, 1.0f, 1.0f };
    std::array<float, 3> angles { 0.0f, 0.0f, 0.0f };
    std::array<float, 2> parallaxDepth { 0.0f, 0.0f };
    std::array<float, 3> color { 1.0f, 1.0f, 1.0f };
    std::string          light;
    float                radius { 1000.0f };
    float                intensity { 1.0f };
    // Per-light falloff power consumed by ComputePBRLightShadow as
    // `radiance = color * pow(saturate(1 - d/radius), exponent)`.
    // Default 1.0 (linear falloff) preserves the pre-2026-05 behavior for
    // legacy scenes that don't author this field.  Real-Time Earth uses 0.1
    // for a soft long-tail falloff that keeps most of the radius lit.
    float                exponent { 1.0f };
    bool                 visible { true };
    // Volumetric-related fields parsed from the light JSON object.  All four
    // are NOWARN-parsed: most authored lights omit them entirely (no opt-in to
    // volumetric contribution), and the warn would flood the logs.
    //
    // density default 0.0f intentionally diverges from the editor's
    // emit-on-toggle default 2.0 — when the JSON omits the field entirely,
    // the light does not participate in volumetrics.  The editor's 2.0
    // serializes when the author actually toggled the volumetric checkbox
    // (those scenes will land with density=2.0 here and opt in via the
    // heuristic).
    float                density { 0.0f };
    float                volumetrics_exponent { 1.0f };
    bool                 cast_shadow { false };
    // cascadedistance0/1/2 — directional-light cascaded shadow split distances.
    // Stored as one packed array; FromJson reads three independent scalars
    // into the three slots.  Defaults match the collisionmodel preview's
    // serialization (0/100/200) so a light that omits the keys round-trips
    // identically to the authored values.
    std::array<float, 3> cascade_distances { 0.0f, 100.0f, 200.0f };
};

} // namespace wpscene
} // namespace wallpaper
/*
        {
            "angles" : "0.00000 0.00000 0.00000",
            "color" : "1.00000 0.95686 0.87451",
            "id" : 237,
            "intensity" : 0.5,
            "light" : "point",
            "locktransforms" : false,
            "name" : "",
            "origin" : "611.30676 302.13736 2000.00000",
            "parallaxDepth" : "0.00000 0.00000",
            "radius" : 3000.0,
            "scale" : "1.00000 1.00000 1.00000",
            "visible" : true
        },
*/
