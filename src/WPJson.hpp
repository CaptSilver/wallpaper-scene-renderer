#pragma once
#include <cstddef>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <optional>
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

// Resource caps applied inside ParseJson — see WPJson.cpp for the SAX gate.
// Cap rationale (2026-05-15 560-scene audit): largest observed scene.json was
// ~1.4MB / ~12-deep / ~200k elements.  64MB / 64 / 5M leaves 25-50x headroom
// over the worst real wallpaper, well clear of false positives but tight
// enough to keep a hostile file from wedging plasmashell.
inline constexpr std::size_t kMaxJsonBytes    = 64 * 1024 * 1024;
inline constexpr std::size_t kMaxJsonDepth    = 64;
inline constexpr std::size_t kMaxJsonElements = 5'000'000;

template<typename T>
struct JsonTemplateTypeCheck {
    using type = bool;
    static_assert(! std::is_const_v<T>, "GetJsonValue need a non const value");
};

template<typename T>
typename wallpaper::JsonTemplateTypeCheck<T>::type
GetJsonValue(const char* file, const char* func, int line, const nlohmann::json& json, T& value,
             bool has_name, std::string_view name, bool warn);

// Parses `source` as lenient JSON (comments + trailing commas + first-key
// quote recovery + leading-zero strip), with hard caps applied before and
// during parse:
//   - source.size() must be < kMaxJsonBytes
//   - nesting depth must not exceed kMaxJsonDepth
//   - total element + key count must not exceed kMaxJsonElements
// Returns false (with LOG_ERROR) on any cap hit, parse error, or
// validation failure; result is left in the default-constructed state.
// Caps apply to every PARSE_JSON call site uniformly — there is no per-
// caller override.  Set WEKDE_DEBUG_JSON_LIMITS=1 to emit a LOG_INFO with
// observed size/depth/element counts on every parse for cap tuning.
bool ParseJson(const char* file, const char* func, int line, const std::string& source,
               nlohmann::json& result);

// Strip trailing commas before `]` / `}` while preserving string literals and
// escape sequences.  nlohmann 3.12 has `ignore_comments` but no
// `ignore_trailing_commas`, and some wallpaper effect JSON files contain
// trailing commas before a closing bracket.  Applied automatically inside
// ParseJson so every call site benefits; also exposed for direct testing.
std::string StripTrailingCommas(std::string_view source);

// Strip leading zeros from number literals (`[0,01]` → `[0,1]`), preserving
// `0.x` decimals and `0` itself.  nlohmann::json (correctly per RFC 8259)
// rejects leading-zero integers; WE accepts them as the workshop publish
// pipeline occasionally writes ranges like `"range":[0,01]` (Astronaut
// 2530355779 ships this in its u_userSpeed annotation).  Applied automatically
// inside ParseJson alongside the other recoveries.
std::string StripLeadingZeros(std::string_view source);

// Resolve a possibly-user-property-bound JSON value WITHOUT copying when no
// resolution is needed.  The dominant case — a value with no "user" key, or no
// active user-property context — returns a reference into `json` directly, so
// the per-field reads in the scene parser do not deep-copy a JSON subtree on
// every GET_JSON_* call.  Only the genuine resolved case (an active
// g_currentUserProperties resolving a {"user":...} field, which may yield a
// synthesized/temporary value) materializes into caller-owned `storage` and
// returns a reference into it.  `storage` must outlive the returned reference.
const nlohmann::json& ResolveUserPropertyRef(const nlohmann::json&          json,
                                             std::optional<nlohmann::json>& storage);

// Quote first-key-missing-opening-quote object members like `{Foo":0,"Bar":1}`,
// rewriting to `{"Foo":0,"Bar":1}`.  WE workshop shader source occasionally
// ships a [COMBO] line where the first key inside an inline `"options":{...}`
// block dropped its opening quote (apparently a serializer artifact in some
// Workshop publish pipelines).  Found across multiple shaders in the 2026-05-15
// audit (Falling Deeper 3061226599, Floating Ducks 3377132665, Lost Cat. 2
// 3356678415, The Blur 3562086244, Final Demons 3216242451, etc.).
// Only fixes the unambiguous `<bracketOrComma><word>"` shape; keys that look
// like other JSON syntax are left untouched.
std::string QuoteFirstKey(std::string_view source);
} // namespace wallpaper
