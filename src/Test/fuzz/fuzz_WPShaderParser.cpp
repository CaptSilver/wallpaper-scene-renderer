// libFuzzer entry point for WPShaderParser::PreShaderSrc.
//
// Targets the text-parsing path: #include scanning, comment-line guarding,
// WE-shader annotation parsing (// [TEXTURE], // [COMBO]). VFS is empty
// — any #include resolves to a no-op, exercising the parser's robustness
// to missing files. CompileToSpv is NOT exercised (it pulls in glslang
// statefulness; needs a separate harness).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Fs/VFS.h"
#include "WPShaderParser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 1u * 1024u * 1024u) return 0;
    std::string                             src(reinterpret_cast<const char*>(data), size);
    wallpaper::fs::VFS                      vfs;
    wallpaper::WPShaderInfo                 info;
    std::vector<wallpaper::WPShaderTexInfo> texs;
    (void)wallpaper::WPShaderParser::PreShaderSrc(vfs, src, &info, texs);
    return 0;
}
