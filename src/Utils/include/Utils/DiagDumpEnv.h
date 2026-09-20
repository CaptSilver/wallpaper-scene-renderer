#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "String.h"

// WEKDE_DIAG_SHARED / WEKDE_DIAG_NODES select which shared.* names / scene
// node ids the periodic diagnostic dumps in SceneBackend.cpp and
// SceneWallpaper.cpp print, replacing a hardcoded per-wallpaper name/id list
// with something any wallpaper's own log line can ask for. Both env vars
// mirror std::getenv()'s shape (pass the raw C string; nullptr means unset),
// the same convention as Vulkan/TextureCacheDetail.hpp's parseQueryCapEnv.
// Header-only pure logic, no Qt/Vulkan dependency, so it's tested without
// pulling in either caller -- same shape as ConsoleFlushDedup.hpp.
namespace utils
{

namespace diag_dump_detail
{
inline std::string trimToken(const std::string& s) {
    const std::size_t begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const std::size_t end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}
} // namespace diag_dump_detail

// Splits on ',' (SpliteString -- the same tokenizer StrToArray::Convert
// already uses), trims surrounding whitespace, drops empty entries: "a, ,b,"
// yields {"a", "b"}, not {"a", "", "b", ""}.
inline std::vector<std::string> parseDiagNameListEnv(const char* env) {
    std::vector<std::string> names;
    if (env == nullptr || env[0] == '\0') return names;
    for (const std::string& piece : SpliteString(env, ',')) {
        std::string trimmed = diag_dump_detail::trimToken(piece);
        if (! trimmed.empty()) names.push_back(std::move(trimmed));
    }
    return names;
}

// Same split/trim as parseDiagNameListEnv, then int-parses each token,
// skipping (not aborting on) a malformed one -- one typo in the list
// shouldn't blank the whole selection. Uses strtol directly rather than
// Utils/String.h's STRTONUM: STRTONUM's contract on a bad token is "log and
// leave the output at a default-constructed 0" (see StrToArray::Convert),
// which can't be told apart from a genuine "0" in the list; strtol's endptr
// gives that signal for free -- the same idiom
// Vulkan/TextureCacheDetail.hpp's parseQueryCapEnv already uses.
inline std::vector<int> parseDiagNodeIdListEnv(const char* env) {
    std::vector<int> ids;
    if (env == nullptr || env[0] == '\0') return ids;
    for (const std::string& piece : SpliteString(env, ',')) {
        const std::string trimmed = diag_dump_detail::trimToken(piece);
        if (trimmed.empty()) continue;
        char*      endp = nullptr;
        const long val  = std::strtol(trimmed.c_str(), &endp, 10);
        if (endp == trimmed.c_str() || *endp != '\0') continue;
        ids.push_back(static_cast<int>(val));
    }
    return ids;
}

// Default behaviour of WEKDE_DIAG_SHARED when unset (SceneBackend.cpp's
// PROPEVAL dump): instead of guessing at a fixed name list, report whichever
// shared vars actually moved since the last time the dump fired. `previous`
// is the caller's snapshot from the last firing (empty -- e.g. right after a
// wallpaper switch -- makes every current var count as "changed", which is
// the right first sample); `current` is this firing's full read of
// shared.*, in enumeration order. Returns at most `maxCount` (name, value)
// pairs, in `current`'s order, and updates `previous` in place to this
// firing's values so the next call diffs against this one, not an older one.
inline std::vector<std::pair<std::string, double>>
selectChangedSharedVars(std::unordered_map<std::string, double>&           previous,
                        const std::vector<std::pair<std::string, double>>& current,
                        std::size_t                                        maxCount) {
    std::vector<std::pair<std::string, double>> changed;
    for (const auto& [name, value] : current) {
        auto it = previous.find(name);
        if ((it == previous.end() || it->second != value) && changed.size() < maxCount) {
            changed.emplace_back(name, value);
        }
    }
    for (const auto& [name, value] : current) previous[name] = value;
    return changed;
}

} // namespace utils
