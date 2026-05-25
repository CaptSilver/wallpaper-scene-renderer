#include "WPLightObject.hpp"

#include "Utils/Logging.h"
#include "Fs/VFS.h"

using namespace wallpaper::wpscene;

bool WPLightObject::FromJson(const nlohmann::json& json, fs::VFS&) {
    GET_JSON_NAME_VALUE(json, "origin", origin);
    GET_JSON_NAME_VALUE(json, "angles", angles);
    GET_JSON_NAME_VALUE(json, "scale", scale);
    GET_JSON_NAME_VALUE(json, "color", color);
    GET_JSON_NAME_VALUE(json, "light", light);
    GET_JSON_NAME_VALUE(json, "radius", radius);
    GET_JSON_NAME_VALUE(json, "intensity", intensity);
    GET_JSON_NAME_VALUE_NOWARN(json, "visible", visible);
    GET_JSON_NAME_VALUE_NOWARN(json, "name", name);
    GET_JSON_NAME_VALUE_NOWARN(json, "parallaxDepth", parallaxDepth);
    // NOWARN — most authored lights are root-attached with linear falloff;
    // only scenes that drive lights from a script-animated parent (Real-Time
    // Earth 3557068717 SUN m5) or want a soft long-tail falloff set these.
    GET_JSON_NAME_VALUE_NOWARN(json, "id", id);
    GET_JSON_NAME_VALUE_NOWARN(json, "parent", parent_id);
    GET_JSON_NAME_VALUE_NOWARN(json, "exponent", exponent);
    // NOWARN — most authored lights omit these volumetric fields entirely;
    // only scenes that opt into the volumetric pipeline (density toggled in
    // the editor, cascaded shadows on directional lights) serialize them.
    GET_JSON_NAME_VALUE_NOWARN(json, "density",             density);
    GET_JSON_NAME_VALUE_NOWARN(json, "volumetricsexponent", volumetrics_exponent);
    GET_JSON_NAME_VALUE_NOWARN(json, "castshadow",          cast_shadow);
    GET_JSON_NAME_VALUE_NOWARN(json, "cascadedistance0",    cascade_distances[0]);
    GET_JSON_NAME_VALUE_NOWARN(json, "cascadedistance1",    cascade_distances[1]);
    GET_JSON_NAME_VALUE_NOWARN(json, "cascadedistance2",    cascade_distances[2]);
    return true;
}
