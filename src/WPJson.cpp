#include "WPJson.hpp"
#include <nlohmann/json.hpp>

#include "Utils/Identity.hpp"
#include "Utils/String.h"
#include "WPUserProperties.hpp"

namespace wallpaper
{

// Resolve user property reference if present
// Returns the resolved JSON value (either from user properties or default value)
static nlohmann::json ResolveUserProperty(const nlohmann::json& json) {
    if (! json.is_object() || ! json.contains("user")) {
        return json;
    }

    if (g_currentUserProperties != nullptr) {
        return g_currentUserProperties->ResolveValue(json);
    }

    // No user properties context, use default value
    if (json.contains("value")) {
        return json["value"];
    }
    return json;
}

std::string StripTrailingCommas(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    bool in_string = false;
    bool escape    = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        char c = source[i];
        if (in_string) {
            out += c;
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            out += c;
            continue;
        }
        if (c == ',') {
            // Peek past ASCII whitespace; if the next char is `]` or `}`,
            // drop the comma.  This misses the (rare) case where a block
            // comment sits between `,` and the closing bracket, but covers
            // everything observed in shipped WE assets.
            std::size_t j = i + 1;
            while (j < source.size()
                   && std::isspace(static_cast<unsigned char>(source[j]))) {
                ++j;
            }
            if (j < source.size() && (source[j] == ']' || source[j] == '}')) {
                continue;
            }
        }
        out += c;
    }
    return out;
}

bool ParseJson(const char* file, const char* func, int line, const std::string& source,
               nlohmann::json& result) {
    try {
        // Pre-strip trailing commas; then let nlohmann handle comments.
        const std::string cleaned = StripTrailingCommas(source);
        result                    = nlohmann::json::parse(cleaned, nullptr, true, true);
    } catch (nlohmann::json::parse_error& e) {
        WallpaperLog(LOGLEVEL_ERROR, file, line, "parse json(%s), %s", func, e.what());
        return false;
    }
    return true;
}

template<typename T>
inline bool _GetJsonValue(const nlohmann::json&                  json,
                          typename utils::is_std_array<T>::type& value) {
    using Tv = typename T::value_type;

    // Resolve user property reference first
    nlohmann::json resolved = ResolveUserProperty(json);

    const auto* pjson = &resolved;
    if (resolved.contains("value")) pjson = &resolved.at("value");
    const auto& njson = *pjson;
    if (njson.is_number()) {
        // Replicate scalar to all components (e.g. uniform scale slider → xyz scale)
        Tv v = njson.get<Tv>();
        if constexpr (requires { value.resize(1); }) {
            value.assign(1, v);
        } else {
            std::fill(value.begin(), value.end(), v);
        }
        return true;
    } else {
        std::string strvalue;
        strvalue = njson.get<std::string>();
        return utils::StrToArray::Convert(strvalue, value);
    }
}

template<typename T>
inline bool _GetJsonValue(const nlohmann::json& json, T& value) {
    // Resolve user property reference first
    nlohmann::json resolved = ResolveUserProperty(json);

    if (resolved.contains("value"))
        value = resolved.at("value").get<T>();
    else
        value = resolved.get<T>();
    return true;
}

template<typename T>
inline bool _GetJsonValue(const char* file, const char* func, int line, const nlohmann::json& json,
                          T& value, bool warn, const char* name) {
    (void)warn;

    using njson = nlohmann::json;
    std::string nameinfo;
    if (name != nullptr) nameinfo = std::string("(key: ") + name + ")";
    try {
        return _GetJsonValue<T>(json, value);
    } catch (const njson::type_error& e) {
        WallpaperLog(LOGLEVEL_INFO,
                     file,
                     line,
                     "%s %s at %s\n%s",
                     e.what(),
                     nameinfo.c_str(),
                     func,
                     json.dump(4).c_str());
    } catch (const std::invalid_argument& e) {
        WallpaperLog(LOGLEVEL_ERROR, file, line, "%s %s at %s", e.what(), nameinfo.c_str(), func);
    } catch (const std::out_of_range& e) {
        WallpaperLog(LOGLEVEL_ERROR, file, line, "%s %s at %s", e.what(), nameinfo.c_str(), func);
    } catch (const utils::StrToArray::WrongSizeExp& e) {
        WallpaperLog(LOGLEVEL_ERROR, file, line, "%s %s at %s", e.what(), nameinfo.c_str(), func);
    }
    return false;
}

template<typename T>
typename JsonTemplateTypeCheck<T>::type
GetJsonValue(const char* file, const char* func, int line, const nlohmann::json& json, T& value,
             bool has_name, std::string_view name_view, bool warn) {
    std::string name { name_view };
    if (has_name) {
        if (! json.contains(name)) {
            if (warn)
                WallpaperLog(LOGLEVEL_INFO,
                             "",
                             0,
                             "read json \"%s\" not a key at %s(%s:%d)",
                             name.data(),
                             func,
                             file,
                             line);
            return false;
        } else if (json.at(name).is_null()) {
            if (warn)
                WallpaperLog(LOGLEVEL_INFO,
                             "",
                             0,
                             "read json \"%s\" is null at %s(%s:%d)",
                             name.data(),
                             func,
                             file,
                             line);
            return false;
        }
    }
    return _GetJsonValue<T>(file,
                            func,
                            line,
                            has_name ? json.at(name) : json,
                            value,
                            warn,
                            name.empty() ? nullptr : name.c_str());
}

#define T_IMPL_GET_JSON(TYPE)                                                            \
    template JsonTemplateTypeCheck<TYPE>::type GetJsonValue<TYPE>(const char*,           \
                                                                  const char*,           \
                                                                  int,                   \
                                                                  const nlohmann::json&, \
                                                                  TYPE&,                 \
                                                                  bool,                  \
                                                                  std::string_view,      \
                                                                  bool);

T_IMPL_GET_JSON(bool);
T_IMPL_GET_JSON(int32_t);
T_IMPL_GET_JSON(uint32_t);
T_IMPL_GET_JSON(float);
T_IMPL_GET_JSON(double);
T_IMPL_GET_JSON(std::string);
T_IMPL_GET_JSON(std::vector<float>);

template<std::size_t N>
using iarray = std::array<int, N>;
T_IMPL_GET_JSON(iarray<3>);

template<std::size_t N>
using farray = std::array<float, N>;
T_IMPL_GET_JSON(farray<2>);
T_IMPL_GET_JSON(farray<3>);

// template bool GetJsonValue();
} // namespace wallpaper
