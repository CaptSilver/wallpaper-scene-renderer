// libFuzzer entry point for WPPkgFs::CreateFromStream + the entry-stream
// layer.
//
// scene.pkg is a binary archive: ver-string + entry-count + per-entry
// (name-string + i32 offset + i32 length). Two distinct stream-driven
// resize sites — name and entry table — both now bounded by CountFitsStream.
//
// Phase 1: drive CreateFromStream with arbitrary bytes (header parsing).
//
// Phase 2: exercise the LimitedBinaryStream layer that .pkg entries
// hand out. The fuzz buffer's first 8 bytes pick a (start, size) slice
// over the rest of the buffer, then we Read + Seek the resulting stream
// with derived patterns. This catches bounds bugs in
// LimitedBinaryStream::SeekSet/SeekCur/SeekEnd + Read clamping that
// the parser-only phase can't reach (the parser's CountFitsStream
// guard sits BEFORE the offset/length pair ever gets used for I/O).

#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory>

#include "Fs/MemBinaryStream.h"
#include "Fs/LimitedBinaryStream.h"
#include "WPPkgFs.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 4u * 1024u * 1024u) return 0;

    // Phase 1 — parser.
    {
        std::vector<uint8_t>           buf(data, data + size);
        wallpaper::fs::MemBinaryStream f(std::move(buf));
        (void)wallpaper::fs::WPPkgFs::CreateFromStream(f, "fuzz.pkg");
    }

    // Phase 2 — LimitedBinaryStream Read/Seek bounds. Need at least 8 bytes
    // of "control" data + 1 byte of payload to make a meaningful slice.
    if (size < 9) return 0;
    const int32_t  start = static_cast<int32_t>(
        (uint32_t)data[0] | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24));
    const int32_t  len = static_cast<int32_t>(
        (uint32_t)data[4] | ((uint32_t)data[5] << 8)
        | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24));

    std::vector<uint8_t>           payload(data + 8, data + size);
    auto backing = std::make_shared<wallpaper::fs::MemBinaryStream>(
        std::move(payload));
    wallpaper::fs::LimitedBinaryStream slice(backing, start, len);

    // Read a chunk, seek around, read again. Each call is bounds-checked
    // internally; the goal is to surface ASan complaints from a Read /
    // Seek arithmetic path that misses an underflow on negative start
    // or oversize len.
    uint8_t out[256];
    (void)slice.Read(out, sizeof(out));
    (void)slice.SeekSet(0);
    (void)slice.SeekCur(static_cast<int32_t>(size & 0xFFFF));
    (void)slice.SeekEnd(-1);
    (void)slice.Read(out, sizeof(out));
    return 0;
}
