// libFuzzer entry point for WPShaderParser::CompileToSpv (the glslang path).
//
// fuzz_WPShaderParser stops at the text-parsing path (PreShaderSrc).  This
// harness drives the actual glslang invocation: it compiles arbitrary GLSL with
// NO cache dir mounted, so CompileToSpv takes the SYNCHRONOUS branch
// (CompileShaderUnits) and returns a bool.  A `false` return (invalid GLSL) is a
// NORMAL outcome — the oracle is the libFuzzer/ASAN/UBSAN runtime: only crashes
// / OOM / sanitizer trips / timeouts are findings.  glslang is the host-CPU
// SPIR-V toolchain (no Vulkan device).
//
// InitGlslang() is per-process and called once below.  We deliberately do NOT
// call FinalGlslang() per iteration — that would tear down the keyword tables
// the g_glslangSerialiseMtx serialization protects (see glslang-thread-safety
// audit / translucent-blend regression history).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Fs/VFS.h"
#include "WPShaderParser.hpp"

using namespace wallpaper;

// One-time glslang init (per-process).  Guarded so it runs exactly once.
static const bool g_glslang_init = [] {
    WPShaderParser::InitGlslang();
    return true;
}();

static ShaderType pickStage(uint8_t b) {
    switch (b % 3) {
    case 0: return ShaderType::VERTEX;
    case 1: return ShaderType::FRAGMENT;
    default: return ShaderType::GEOMETRY; // exercises TranslateGeometryShader + layout inject
    }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    (void)g_glslang_init;
    if (size > 512u * 1024u) return 0; // renderer shaders are small
    std::string src(reinterpret_cast<const char*>(data), size);

    fs::VFS      vfs;  // NOT mounted "cache" -> synchronous compile branch
    WPShaderInfo info; // empty combos/svs/alias/defTexs

    std::vector<WPShaderUnit> units;
    units.push_back(WPShaderUnit { pickStage(size ? data[0] : 0), src, {} });

    std::vector<ShaderCode>      codes;
    std::vector<WPShaderTexInfo> texs;
    (void)WPShaderParser::CompileToSpv("fuzz", units, codes, vfs, &info, texs);
    return 0; // crash / OOM / UBSAN trip == finding
}
