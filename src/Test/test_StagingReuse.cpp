#include <doctest.h>

#include "Vulkan/StagingReuse.hpp"

using wallpaper::vulkan::stagingBufferReusable;

TEST_CASE("staging buffer is reused only when present and large enough") {
    CHECK(stagingBufferReusable(true, 100, 100));        // exact fit -> reuse
    CHECK(stagingBufferReusable(true, 200, 100));        // larger    -> reuse
    CHECK_FALSE(stagingBufferReusable(true, 50, 100));   // too small -> recreate
    CHECK_FALSE(stagingBufferReusable(false, 200, 100)); // no buffer -> create
}
