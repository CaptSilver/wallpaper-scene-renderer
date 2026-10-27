#pragma once

#include <span>
#include <string>
#include <string_view>
#include "Scene/Scene.h"
#include "Scene/SceneShader.h"
#include "Type.hpp"

namespace wallpaper
{
namespace fs
{
class VFS;
}
using Combos = Map<std::string, std::string>;

// ui material name to gl uniform name
using WPAliasValueDict = Map<std::string, std::string>;

using WPDefaultTexs = std::vector<std::pair<i32, std::string>>;

struct WPShaderInfo {
    Combos           combos;
    ShaderValueMap   svs;
    ShaderValueMap   baseConstSvs;
    WPAliasValueDict alias;
    WPDefaultTexs    defTexs;
};

struct WPPreprocessorInfo {
    Map<std::string, std::string> input; // name to line
    Map<std::string, std::string> output;

    Set<uint> active_tex_slots;
};

struct WPShaderTexInfo {
    bool                enabled { false };
    std::array<bool, 4> composEnabled { false, false, false, false };
};

struct WPShaderUnit {
    ShaderType         stage;
    std::string        src;
    WPPreprocessorInfo preprocess_info;
};

// Identity of the shader translator this binary was built with: a SHA-1 over the
// sources that decide the translated SPIR-V (see cmake/WekShaderFingerprint.cmake).
std::string_view ShaderTranslatorFingerprint();

// Key of a compiled-SPIR-V cache entry.  The units are hashed before
// translation runs, so `salt` is what ties an entry to the translator that
// produced it; a different salt never reuses another translator's entry.
std::string ShaderCacheKey(std::span<const WPShaderUnit> units,
                           std::string_view              salt = ShaderTranslatorFingerprint());

class WPShaderParser {
public:
    static std::string PreShaderSrc(fs::VFS&, const std::string& src, WPShaderInfo* pWPShaderInfo,
                                    const std::vector<WPShaderTexInfo>& texs);

    static std::string PreShaderHeader(const std::string& src, const Combos& combos, ShaderType);

    static void InitGlslang();
    static void FinalGlslang();

    // Compile `units` to SPIR-V into `spvs`, going through the on-disk SPV cache
    // when a "cache" mount exists.  The SPV is published into `spvs` BEFORE this
    // returns and nothing about `spvs` is remembered afterwards, so a scene load
    // that dies mid-parse leaves nothing here pointing at the buffers it freed.
    static bool CompileToSpv(std::string_view         scene_id, std::span<WPShaderUnit>,
                             std::vector<ShaderCode>& spvs, fs::VFS&, WPShaderInfo*,
                             std::span<const WPShaderTexInfo>);

    // Drop the in-memory sha1 -> SPV memo CompileToSpv builds up, and log the
    // compile summary for the batch.  Call at end-of-parse: the SPV is already
    // on disk by then, and holding it costs a few MB per wallpaper that would
    // otherwise accumulate for the life of the process.
    static void ClearSpvMemo();

    /// Number of shader compiles the memo has actually performed since the last
    /// ClearSpvMemo().  Exists so a test can assert that a source shared by two
    /// materials is compiled once rather than twice -- the disk cache shows one
    /// entry either way, so it cannot tell dedupe from a redundant recompile.
    static std::size_t MemoCompileCount();
};
} // namespace wallpaper
