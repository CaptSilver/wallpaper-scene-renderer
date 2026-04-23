#pragma once
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <string_view>
#include <type_traits>

#include "Utils/Logging.h"

#define GET_JSON_VALUE(json, value) \
    wallpaper::GetJsonValue(        \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), false, "", true)
#define GET_JSON_NAME_VALUE(json, name, value) \
    wallpaper::GetJsonValue(                   \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), true, (name), true)

#define GET_JSON_VALUE_NOWARN(json, value) \
    wallpaper::GetJsonValue(               \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), false, "", false)
#define GET_JSON_NAME_VALUE_NOWARN(json, name, value) \
    wallpaper::GetJsonValue(                          \
        __SHORT_FILE__, __FUNCTION__, __LINE__, (json), (value), true, (name), false)

#define PARSE_JSON(source, result) \
    wallpaper::ParseJson(__SHORT_FILE__, __FUNCTION__, __LINE__, (source), (result))

namespace wallpaper
{

template<typename T>
struct JsonTemplateTypeCheck {
    using type = bool;
    static_assert(! std::is_const_v<T>, "GetJsonValue need a non const value");
};

template<typename T>
typename wallpaper::JsonTemplateTypeCheck<T>::type
GetJsonValue(const char* file, const char* func, int line, const nlohmann::json& json, T& value,
             bool has_name, std::string_view name, bool warn);

bool ParseJson(const char* file, const char* func, int line, const std::string& source,
               nlohmann::json& result);

// Strip trailing commas before `]` / `}` while preserving string literals and
// escape sequences.  nlohmann 3.12 has `ignore_comments` but no
// `ignore_trailing_commas` — WE's own loader is lax about both, and at least
// one shipped asset (assets/effects/fluidsimulation/effect.json) has a
// trailing comma before `]`.  Applied automatically inside ParseJson so every
// call site benefits; also exposed for direct testing.
std::string StripTrailingCommas(std::string_view source);
} // namespace wallpaper
