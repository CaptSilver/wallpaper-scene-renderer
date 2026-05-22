#pragma once

#include <span>
#include <string>
#include <string_view>

#include "Type.hpp"        // wallpaper::ShaderType
#include "Utils/Sha.hpp"   // utils::genSha1
#include "Core/MapSet.hpp" // wallpaper::Map (Combos is Map<std::string,std::string>)

namespace wallpaper
{

// Bump to invalidate every on-disk SPV entry in one move.  Increment whenever
// the preprocess/compile pipeline changes in a way that alters output for the
// SAME raw inputs (a WPShaderTransforms.h transform, a preamble change, a
// glslang client-version change).  This is the principled replacement for the
// hand wipe of the per-scene shader cache dir the cache used to require to pick
// up such pipeline edits.
inline constexpr unsigned kShaderCacheKeyVersion = 1;

struct ShaderCacheUnitInput {
    ShaderType       stage;
    std::string_view raw_src; // BEFORE Preprocessor (post geometry-translate)
};

// Internal: build the key with an explicit version (test seam for the bump).
inline std::string shaderCacheKeyWithVersion(std::span<const ShaderCacheUnitInput> units,
                                             const Map<std::string, std::string>&  combos,
                                             unsigned                              version) {
    // Control-char separators (US/RS/GS) cannot appear in shader source that
    // matters here nor in a combo name (an uppercased identifier) or its
    // decimal value, so they unambiguously delimit fields/records.  Per-unit
    // sources are pre-hashed to a fixed 40-char digest, removing any
    // length-ambiguity between a source that contains a separator and a real
    // unit boundary.  unit count + per-record separators stop {ab} colliding
    // with {a,b}.  combos iterate in std::map order -> order-independent.
    std::string acc;
    acc += std::to_string(version);
    acc += '\x1f';
    acc += std::to_string(units.size());
    for (const auto& u : units) {
        acc += '\x1e';
        acc += std::to_string(static_cast<int>(u.stage));
        acc += '\x1f';
        acc += utils::genSha1({ u.raw_src.data(), u.raw_src.size() });
    }
    for (const auto& [k, v] : combos) {
        acc += '\x1d';
        acc += k;
        acc += '\x1f';
        acc += v;
    }
    return utils::genSha1({ acc.data(), acc.size() });
}

// The shipped key: discriminates the SPV disk cache on the inputs that fully
// determine the compiled shader -- each unit's raw source + stage, and the
// combos dict -- so a warm cache can be consulted WITHOUT running Preprocessor
// + glslang-preprocess.  Deterministic; combos order-independent.
inline std::string shaderCacheKey(std::span<const ShaderCacheUnitInput> units,
                                  const Map<std::string, std::string>&  combos) {
    return shaderCacheKeyWithVersion(units, combos, kShaderCacheKeyVersion);
}

} // namespace wallpaper
