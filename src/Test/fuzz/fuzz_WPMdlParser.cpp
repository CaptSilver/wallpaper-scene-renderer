// libFuzzer entry point for WPMdlParser::ParseStream.
//
// Build (from project root, inside distrobox):
//   cmake -S src/backend_scene -B build/fuzz \
//         -DBUILD_TESTS=ON -DBUILD_FUZZERS=ON \
//         -DCMAKE_BUILD_TYPE=Debug \
//         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build/fuzz --target fuzz_WPMdlParser
//
// Seed a corpus from real .mdl files (workshop scene.pkg archives) and run:
//   ./build/fuzz/src/Test/fuzz_WPMdlParser corpus/ \
//       -max_total_time=300 -timeout=5 -max_len=65536
//
// Each input is fed to the parser as a synthetic IBinaryStream. ASAN+UBSAN
// surface any read past EOF, signed overflow on a header field, allocator
// abuse, or alignment violation; libFuzzer's coverage feedback drives
// inputs deeper into the version-branched parse paths (mdlv<16, [16,23),
// >=23) without hand-curated cases.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Fs/MemBinaryStream.h"
#include "WPMdlParser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::vector<uint8_t>           buf(data, data + size);
    wallpaper::fs::MemBinaryStream f(std::move(buf));
    wallpaper::WPMdl               mdl;
    (void)wallpaper::WPMdlParser::ParseStream(f, "fuzz.mdl", mdl);
    return 0;
}
