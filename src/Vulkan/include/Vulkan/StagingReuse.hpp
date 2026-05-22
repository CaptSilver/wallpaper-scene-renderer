#pragma once
#include <cstddef>

namespace wallpaper::vulkan
{

// A cached staging buffer can be reused for a new upload iff it already exists
// and is at least as large as the new payload.  Video frames are fixed-size, so
// after the first frame every re-upload reuses; a grown payload forces a
// recreate.  Pure so the reuse policy is unit-testable without a device.
inline bool stagingBufferReusable(bool hasHandle, std::size_t haveReqSize, std::size_t needSize) {
    return hasHandle && haveReqSize >= needSize;
}

} // namespace wallpaper::vulkan
