#pragma once

// Coercing a resolved user-property JSON value to the float vector a shader
// constant holds.  The parser (usershadervalues) and the live uniform update
// share it, so a property reads the same at parse time and when it changes.
//
// Kept apart from UserPropRuntimeUpdate.hpp, which pulls in Scene/Scene.h; the
// material parser sits below the scene layer.

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace wek
{

// A bool becomes 1 or 0, a bare number one element, a string is
// whitespace-split into floats, an array keeps only its numeric elements.
inline std::vector<float> ParseFloatVectorFromJson(const nlohmann::json& val) {
    std::vector<float> floatVec;
    if (val.is_boolean()) {
        floatVec.push_back(val.get<bool>() ? 1.0f : 0.0f);
    } else if (val.is_number()) {
        floatVec.push_back(val.get<float>());
    } else if (val.is_string()) {
        std::istringstream iss(val.get<std::string>());
        float              f;
        while (iss >> f) floatVec.push_back(f);
    } else if (val.is_array()) {
        for (const auto& elem : val) {
            if (elem.is_number()) floatVec.push_back(elem.get<float>());
        }
    }
    return floatVec;
}

} // namespace wek
