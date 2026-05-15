#pragma once
#include <string_view>
#include <charconv>
#include "Fs/IBinaryStream.h"
#include "Utils/Logging.h"
#include "Core/StringHelper.hpp"

namespace wallpaper
{

// Compute the ortho camera's (width, height) from scene.json's `general.zoom`
// and the scene's ortho size.  WE's zoom is a divisor: 1 → native; 0.5 → camera
// sees 2× the world (zoomed out); 2 → camera sees half (zoomed in).
//
// Pre-fix, ParseCamera ran `context.ortho_w / (i32)general.zoom` which:
//   * Truncates 0.5 to 0 → integer division by zero → SIGFPE on the load
//     thread → silent process death.
//   * Loses fractional precision even when non-zero (zoom=1.5 → cast to 1).
//
// Found by the 2026-05-15 audit on Spiraling Sunlight (1850341785) and
// Astronaut (2530355779) — both ship `general.zoom` as a user-property slider
// with default 0.5.  Multi-monitor 5760×1080 ortho is incidental.
//
// Returns positive ints; falls back to the native ortho size when the divisor
// is non-positive or the quotient rounds to zero.
inline std::pair<int32_t, int32_t>
ComputeOrthoCameraSize(int32_t ortho_w, int32_t ortho_h, float zoom) {
    const float zoom_div = zoom > 0.0f ? zoom : 1.0f;
    int32_t     cam_w    = static_cast<int32_t>(static_cast<float>(ortho_w) / zoom_div);
    int32_t     cam_h    = static_cast<int32_t>(static_cast<float>(ortho_h) / zoom_div);
    if (cam_w <= 0) cam_w = ortho_w;
    if (cam_h <= 0) cam_h = ortho_h;
    return { cam_w, cam_h };
}

inline int32_t ReadVersion(std::string_view prefix, fs::IBinaryStream& file) {
    char str_v[9] { '\0' };
    file.Read(str_v, 9);
    // Bound the view explicitly: Read() may have filled all 9 bytes with
    // non-null content, so decaying str_v to const char* (as the implicit
    // string_view ctor does) would strlen past the end of the stack array.
    const std::string_view ver(str_v, sizeof(str_v));
    if (! sstart_with(ver, prefix)) return 0;

    char* str_int = str_v + 4;
    int   slot;
    auto [ptr, ec] { std::from_chars(str_int, std::end(str_v), slot) };
    if (ec != std::errc()) {
        LOG_ERROR("read version of \'%.*s\' failed", 8, str_v);
        return 0;
    }
    return slot;
}
inline void WriteVersion(std::string_view prefix, fs::IBinaryStreamW& file, int ver) {
    char buf[9] { '\0' };
    std::snprintf(buf, sizeof(buf), "%.4s%.4d", prefix.data(), ver);
    file.Write(buf, sizeof(buf));
}

inline int32_t ReadTexVesion(fs::IBinaryStream& file) { return ReadVersion("TEX", file); }
inline int32_t ReadMDLVesion(fs::IBinaryStream& file) { return ReadVersion("MDL", file); }

// Sanity-check a stream-derived element count before vector::resize. Hostile
// inputs declare counts (e.g., 0xFFFFFFFF) whose allocation OOMs the process
// long before any subsequent read fails. Each entry consumes at least
// `min_stride_bytes` from the stream, so `count` cannot exceed the bytes
// remaining divided by the stride. Default stride 1 is the conservative
// upper bound when the per-entry size has variable parts (strings, nested
// blocks); tighter strides (e.g., sizeof(Vertex)) can be passed at sites
// where the layout is fully fixed.
inline bool CountFitsStream(const fs::IBinaryStream& file, std::size_t count,
                            std::size_t min_stride_bytes = 1) noexcept {
    if (min_stride_bytes == 0) return false;
    auto pos  = file.Tell();
    auto size = file.Size();
    if (pos < 0 || size < 0 || pos > size) return false;
    auto remaining = static_cast<std::size_t>(size - pos);
    return count <= remaining / min_stride_bytes;
}

// DIY
inline int32_t ReadSPVVesion(fs::IBinaryStream& file) { return ReadVersion("SPV", file); }
inline void WriteSPVVesion(fs::IBinaryStreamW& file, int ver) { WriteVersion("SPVS", file, ver); }

} // namespace wallpaper
