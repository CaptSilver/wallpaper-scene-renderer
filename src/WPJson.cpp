#include "WPJson.hpp"
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Utils/Identity.hpp"
#include "Utils/String.h"
#include "WPUserProperties.hpp"

namespace wallpaper
{

namespace
{

// True when WEKDE_DEBUG_JSON_LIMITS env var is set to a non-empty, non-"0"
// value at first call.  Per project preference, the diagnostic stays in the
// shippable build so future-wallpaper investigation can re-enable observation
// without a rebuild.
bool JsonLimitDebugEnabled() {
    static const bool s_enabled = [] {
        const char* v = std::getenv("WEKDE_DEBUG_JSON_LIMITS");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return s_enabled;
}

// SAX consumer that builds the nlohmann::json DOM while enforcing the depth
// and element-count caps from WPJson.hpp.  Returning false from any callback
// aborts the parse; the caller maps the abort to a LOG_ERROR + early return.
//
// We subclass the public json_sax<json> base (rather than the private
// detail::json_sax_dom_parser) so a future nlohmann version cannot silently
// move the DOM builder under our feet — the public SAX contract is stable.
struct LimitedSaxBuilder : nlohmann::json_sax<nlohmann::json> {
    using json = nlohmann::json;

    explicit LimitedSaxBuilder(json& r) : root(r) {}

    bool null() override                                     { return handle_value(nullptr); }
    bool boolean(bool v) override                            { return handle_value(v); }
    bool number_integer(number_integer_t v) override         { return handle_value(v); }
    bool number_unsigned(number_unsigned_t v) override       { return handle_value(v); }
    bool number_float(number_float_t v, const string_t&) override { return handle_value(v); }
    bool string(string_t& v) override                        { return handle_value(v); }
    bool binary(binary_t& v) override                        { return handle_value(std::move(v)); }

    bool start_object(std::size_t /*len*/) override {
        ++depth;
        if (depth > max_depth_seen) max_depth_seen = depth;
        if (depth > kMaxJsonDepth) return false;
        if (! bump_elements()) return false;
        json* slot = handle_value_slot(json::value_t::object);
        if (slot == nullptr) return false;
        ref_stack.push_back(slot);
        return true;
    }

    bool key(string_t& v) override {
        if (! bump_elements()) return false;
        if (ref_stack.empty() || ! ref_stack.back()->is_object()) return false;
        object_element = &((*ref_stack.back())[v]);
        return true;
    }

    bool end_object() override {
        if (ref_stack.empty()) return false;
        ref_stack.pop_back();
        --depth;
        return true;
    }

    bool start_array(std::size_t /*len*/) override {
        ++depth;
        if (depth > max_depth_seen) max_depth_seen = depth;
        if (depth > kMaxJsonDepth) return false;
        if (! bump_elements()) return false;
        json* slot = handle_value_slot(json::value_t::array);
        if (slot == nullptr) return false;
        ref_stack.push_back(slot);
        return true;
    }

    bool end_array() override {
        if (ref_stack.empty()) return false;
        ref_stack.pop_back();
        --depth;
        return true;
    }

    bool parse_error(std::size_t, const std::string&,
                     const nlohmann::detail::exception& ex) override {
        // Capture the underlying lexer/parser message so ParseJson can log
        // exactly what nlohmann's recursive-descent path would have logged
        // pre-cap.  We don't re-throw here (the virtual override receives
        // the polymorphic base reference, which would slice on re-throw);
        // returning false aborts sax_parse cleanly.
        had_parse_error = true;
        parse_error_msg = ex.what();
        return false;
    }

    bool depth_exceeded() const { return max_depth_seen > kMaxJsonDepth; }
    bool elements_exceeded() const { return elements > kMaxJsonElements; }
    bool had_syntax_error() const { return had_parse_error; }
    const std::string& syntax_error_message() const { return parse_error_msg; }
    std::size_t observed_depth() const { return max_depth_seen; }
    std::size_t observed_elements() const { return elements; }

private:
    template<typename Value>
    bool handle_value(Value&& v) {
        if (! bump_elements()) return false;
        if (ref_stack.empty()) {
            root = json(std::forward<Value>(v));
            return true;
        }
        if (ref_stack.back()->is_array()) {
            ref_stack.back()->emplace_back(std::forward<Value>(v));
            return true;
        }
        if (object_element != nullptr) {
            *object_element = json(std::forward<Value>(v));
            object_element  = nullptr;
            return true;
        }
        return false;
    }

    // For containers: the slot has to be carved out before the elements
    // start flowing so nested handle_value calls can push into the right
    // container.  Returns a pointer to the newly-installed container, or
    // nullptr on a structural error.
    json* handle_value_slot(json::value_t kind) {
        if (ref_stack.empty()) {
            root = json(kind);
            return &root;
        }
        if (ref_stack.back()->is_array()) {
            ref_stack.back()->emplace_back(kind);
            return &ref_stack.back()->back();
        }
        if (object_element != nullptr) {
            *object_element = json(kind);
            json* slot      = object_element;
            object_element  = nullptr;
            return slot;
        }
        return nullptr;
    }

    bool bump_elements() {
        ++elements;
        return elements <= kMaxJsonElements;
    }

    json&              root;
    std::vector<json*> ref_stack {};
    json*              object_element { nullptr };
    std::size_t        depth { 0 };
    std::size_t        max_depth_seen { 0 };
    std::size_t        elements { 0 };
    bool               had_parse_error { false };
    std::string        parse_error_msg;
};

} // namespace

// Resolve user property reference if present.  Returns a reference into `json`
// for the pass-through cases (the common path — zero copy); for the genuine
// resolved case the value is materialized into `storage` (it may be a
// synthesized temporary) and a reference into it is returned, so it does not
// dangle.  See WPJson.hpp for the contract.
const nlohmann::json& ResolveUserPropertyRef(const nlohmann::json&          json,
                                             std::optional<nlohmann::json>& storage) {
    if (! json.is_object() || ! json.contains("user")) {
        return json; // common case: reference into the input, no copy
    }

    if (g_currentUserProperties != nullptr) {
        // The only path that may produce a temporary (ResolveValue can return a
        // synthesized bool or a copy out of GetProperty's optional).  Park it in
        // caller-owned storage and hand back a reference into that slot.
        storage = g_currentUserProperties->ResolveValue(json);
        return *storage;
    }

    // No user-property context: fall back to the embedded default.
    // json["value"] is a reference into `json` (const operator[]), still no copy.
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

std::string QuoteFirstKey(std::string_view source) {
    // Walk source; outside string literals, when we see `{` or `,`, check whether
    // the next non-whitespace run of identifier characters is followed by `"` (the
    // close-quote of an unquoted first key).  If so, inject the missing leading
    // `"` before that identifier run.  Identifier chars accepted match what WE's
    // workshop publish pipeline observed to produce: letters, digits, `_`, ` `,
    // and `+` (e.g. "Noise + mirrored").  Anything else terminates the candidate
    // run and we leave the source untouched.
    std::string out;
    out.reserve(source.size() + 16);
    bool in_string = false;
    bool escape    = false;
    const auto is_ident = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ' ' || c == '+';
    };
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
        out += c;
        if (c == '{' || c == ',') {
            // Skip whitespace
            std::size_t j = i + 1;
            while (j < source.size()
                   && std::isspace(static_cast<unsigned char>(source[j]))) {
                ++j;
            }
            // Identifier must start with a letter or underscore (not a digit,
            // not `+`/space — those are only allowed mid-identifier).
            if (j < source.size()
                && (std::isalpha(static_cast<unsigned char>(source[j])) || source[j] == '_')) {
                std::size_t k = j;
                while (k < source.size() && is_ident(source[k])) ++k;
                // Pattern matches only when the identifier run is followed by
                // a closing `"` (the recovered other half of the key quote).
                if (k < source.size() && source[k] == '"') {
                    // Emit whitespace verbatim, then injected `"`, then the identifier
                    // run as-is.  The closing `"` in source is preserved as we keep
                    // walking from `i+1` after returning.
                    for (std::size_t w = i + 1; w < j; ++w) out += source[w];
                    out += '"';
                    for (std::size_t w = j; w < k; ++w) out += source[w];
                    i = k - 1; // outer loop will i++ to put us at the closing `"`
                }
            }
        }
    }
    return out;
}

std::string StripLeadingZeros(std::string_view source) {
    // Walk source; outside string literals, scan for number literals that
    // start with `0` immediately followed by another digit — those are
    // workshop publish artifacts (`[0,01]`) and not valid JSON.  Strip the
    // leading zero so the runtime keeps the rest of the digits.
    //
    // The "start of a number" position is identified by the preceding char
    // being one of `[`, `,`, `:`, `(`, `+`, `-`, or whitespace.  Anywhere
    // else, `0` followed by a digit is part of a longer token we shouldn't
    // touch (e.g. inside an identifier).
    std::string out;
    out.reserve(source.size());
    bool in_string = false;
    bool escape    = false;
    auto can_start_number = [](char c) {
        return c == '[' || c == ',' || c == ':' || c == '(' || c == '+' ||
               c == '-' || std::isspace(static_cast<unsigned char>(c));
    };
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
        // At number-start boundary, look for `0` followed by another digit.
        // Skip the leading zero in that case, but keep `0`, `0.x`, `0e…`.
        if (c == '0' && i + 1 < source.size() &&
            std::isdigit(static_cast<unsigned char>(source[i + 1])) &&
            (out.empty() || can_start_number(out.back()))) {
            // Drop this leading zero — the next iteration will emit the
            // following digit naturally.
            continue;
        }
        out += c;
    }
    return out;
}

bool ParseJson(const char* file, const char* func, int line, const std::string& source,
               nlohmann::json& result) {
    if (JsonLimitDebugEnabled()) {
        WallpaperLog(LOGLEVEL_INFO,
                     file,
                     line,
                     "ParseJson enter size=%zu func=%s",
                     source.size(),
                     func);
    }

    // Cheap byte gate before any cleanup allocation — a 2GB hostile source
    // is rejected without ever materialising a cleaned copy.
    if (source.size() > kMaxJsonBytes) {
        WallpaperLog(LOGLEVEL_ERROR,
                     file,
                     line,
                     "parse json(%s) rejected: %zu bytes > %zu cap",
                     func,
                     source.size(),
                     kMaxJsonBytes);
        return false;
    }

    try {
        // Pre-strip trailing commas; quote any first-key-missing-opening-quote
        // members ({Foo":0 → {"Foo":0); strip illegal leading zeros from
        // numeric literals ([0,01] → [0,1]); then let nlohmann handle comments.
        const std::string cleaned =
            StripLeadingZeros(QuoteFirstKey(StripTrailingCommas(source)));
        result = nlohmann::json {};
        LimitedSaxBuilder sax(result);
        const bool        ok = nlohmann::json::sax_parse(cleaned,
                                                  &sax,
                                                  nlohmann::json::input_format_t::json,
                                                  /*strict=*/true,
                                                  /*ignore_comments=*/true);
        if (! ok) {
            // sax_parse returns false either when a SAX callback returned
            // false (our cap rejection) or when the lexer/parser hit a
            // syntax error (we captured the message via parse_error()).
            if (sax.depth_exceeded() || sax.elements_exceeded()) {
                WallpaperLog(LOGLEVEL_ERROR,
                             file,
                             line,
                             "parse json(%s) rejected: depth or element cap exceeded "
                             "(kMaxJsonDepth=%zu kMaxJsonElements=%zu observed depth=%zu "
                             "elements=%zu)",
                             func,
                             kMaxJsonDepth,
                             kMaxJsonElements,
                             sax.observed_depth(),
                             sax.observed_elements());
            } else if (sax.had_syntax_error()) {
                WallpaperLog(LOGLEVEL_ERROR,
                             file,
                             line,
                             "parse json(%s), %s",
                             func,
                             sax.syntax_error_message().c_str());
            } else {
                // Defensive: SAX aborted without flagging either case (e.g.
                // a structural inconsistency caught by handle_value_slot).
                WallpaperLog(LOGLEVEL_ERROR,
                             file,
                             line,
                             "parse json(%s) failed: sax abort without diagnosis",
                             func);
            }
            result = nlohmann::json {};
            return false;
        }
        if (JsonLimitDebugEnabled()) {
            WallpaperLog(LOGLEVEL_INFO,
                         file,
                         line,
                         "ParseJson ok size=%zu depth=%zu elements=%zu func=%s",
                         source.size(),
                         sax.observed_depth(),
                         sax.observed_elements(),
                         func);
        }
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

    // Resolve user property reference first (no copy unless a user property
    // actually resolves; see ResolveUserPropertyRef).
    std::optional<nlohmann::json> resolvedStorage;
    const nlohmann::json&         resolved = ResolveUserPropertyRef(json, resolvedStorage);

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
    // Resolve user property reference first (no copy unless a user property
    // actually resolves; see ResolveUserPropertyRef).
    std::optional<nlohmann::json> resolvedStorage;
    const nlohmann::json&         resolved = ResolveUserPropertyRef(json, resolvedStorage);

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
T_IMPL_GET_JSON(farray<4>);

// template bool GetJsonValue();
} // namespace wallpaper
