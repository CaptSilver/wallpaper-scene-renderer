// libFuzzer entry point for WPPkgFs::CreateFromStream.
//
// scene.pkg is a binary archive: ver-string + entry-count + per-entry
// (name-string + i32 offset + i32 length). Two distinct stream-driven
// resize sites — name and entry table — both now bounded by CountFitsStream.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Fs/MemBinaryStream.h"
#include "WPPkgFs.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 4u * 1024u * 1024u) return 0;
    std::vector<uint8_t>           buf(data, data + size);
    wallpaper::fs::MemBinaryStream f(std::move(buf));
    (void)wallpaper::fs::WPPkgFs::CreateFromStream(f, "fuzz.pkg");
    return 0;
}
