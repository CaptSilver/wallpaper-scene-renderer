#pragma once
// Text scans WPShaderParser runs over every shader source on every scene load,
// even when the SPIR-V cache hits.  They used to walk std::regex over the
// whole source (sregex_iterator / repeated regex_search), which showed up as
// ~30% of scene-load time in profiles.  These versions return exactly what
// the regex walks returned; test_ShaderSourceScan.cpp pins that against the
// original regex code.
#include <algorithm>
#include <cstddef>
#include <regex>
#include <string>

namespace wallpaper
{

// First `#if` or `#endif` at or after pos, npos when there is none: what
// regex_search(begin + pos, end, "(#if|#endif)") found.  Both are literals,
// so two finds do it (find already returns npos for pos at or past the end).
inline std::size_t FindIfOrEndif(const std::string& src, std::size_t pos) {
    return std::min(src.find("#if", pos), src.find("#endif", pos));
}

// Start of the last line that begins with `attribute `, `varying ` or
// `uniform ` after a newline (one past that `\n`); 0 when there is none,
// npos when that position is the end of the string.  Same contract as the
// old searchLast(src, "\n(attribute|varying|uniform) ").
inline std::size_t FindLastDeclarationLine(const std::string& src) {
    std::size_t last = std::string::npos;
    for (std::size_t i = src.find('\n'); i != std::string::npos; i = src.find('\n', i + 1)) {
        if (src.compare(i + 1, 10, "attribute ") == 0 || src.compare(i + 1, 8, "varying ") == 0 ||
            src.compare(i + 1, 8, "uniform ") == 0)
            last = i;
    }
    const std::size_t start = last == std::string::npos ? 0 : last + 1;
    return start >= src.size() ? std::string::npos : start;
}

namespace shader_scan_detail
{
// regex_search anchored at `at` (match_continuous), with the character before
// `at` visible to ^ and \b as it is for sregex_iterator's later matches.
inline bool MatchAt(const std::string& src, std::size_t at, std::smatch& m, const std::regex& re) {
    auto flags = std::regex_constants::match_continuous;
    if (at > 0) flags |= std::regex_constants::match_prev_avail;
    return std::regex_search(src.cbegin() + (std::ptrdiff_t)at, src.cend(), m, re, flags);
}
} // namespace shader_scan_detail

// Stage inputs/outputs: calls fn(match) for every match of
//   (?:^|\s)(in|out)\s[\s\w]+\s(\w+)\s*(?:\[\])?\s*;
// in the order sregex_iterator yields them.  A match can only start on the
// `in`/`out` keyword or on the whitespace/line start one character before it,
// so the regex is only tried there, anchored, and the scan steps past each
// match as the iterator does.  keywordAt is only that pre-filter: weakening
// it changes how often the regex runs, never what it matches.
template<typename Fn>
void ForEachStageIoDecl(const std::string& src, Fn&& fn) {
    thread_local const std::regex re(R"((?:^|\s)(in|out)\s[\s\w]+\s(\w+)\s*(?:\[\])?\s*;)",
                                     std::regex::ECMAScript | std::regex::multiline);
    auto                          keywordAt = [&src](std::size_t at) {
        return src.compare(at, 2, "in") == 0 || src.compare(at, 3, "out") == 0;
    };
    for (std::size_t at = 0; at < src.size();) {
        std::smatch m;
        if ((keywordAt(at) || keywordAt(at + 1)) && shader_scan_detail::MatchAt(src, at, m, re)) {
            fn(m);
            at += (std::size_t)std::max<std::ptrdiff_t>(m.length(0), 1);
        } else {
            at++;
        }
    }
}

// Texture uniforms: calls fn(match) for every match of
//   uniform\s+sampler2D\s+g_Texture(\d+)
// in sregex_iterator order.  Every match starts with the literal `uniform`,
// so only those positions are tried.
template<typename Fn>
void ForEachTextureSlotUniform(const std::string& src, Fn&& fn) {
    thread_local const std::regex re(R"(uniform\s+sampler2D\s+g_Texture(\d+))",
                                     std::regex::ECMAScript);
    for (std::size_t at = src.find("uniform"); at != std::string::npos;) {
        std::smatch m;
        if (! shader_scan_detail::MatchAt(src, at, m, re)) {
            at = src.find("uniform", at + 1);
            continue;
        }
        at = src.find("uniform", at + (std::size_t)m.length(0));
        fn(m);
    }
}

} // namespace wallpaper
