#include <doctest.h>

#include "VulkanRender/FramebufferCache.hpp"

using wallpaper::vulkan::FramebufferCache;

TEST_CASE("FramebufferCache creates on miss and reuses on hit") {
    FramebufferCache<int, int> cache;
    int                        creates = 0;
    auto                       make    = [&] {
        return ++creates * 10;
    };

    int& a = cache.getOrCreate(1, make);
    CHECK(creates == 1);
    CHECK(a == 10);

    int& b = cache.getOrCreate(1, make); // same key: must NOT rebuild
    CHECK(creates == 1);
    CHECK(b == 10);
    CHECK(cache.size() == 1);

    cache.getOrCreate(2, make); // different key: rebuilds
    CHECK(creates == 2);
    CHECK(cache.size() == 2);
}

TEST_CASE("FramebufferCache clear() forces recreation") {
    FramebufferCache<int, int> cache;
    int                        creates = 0;
    auto                       make    = [&] {
        return ++creates;
    };

    cache.getOrCreate(7, make);
    CHECK(creates == 1);
    CHECK(cache.contains(7));

    cache.clear();
    CHECK(cache.size() == 0);
    CHECK_FALSE(cache.contains(7));

    cache.getOrCreate(7, make); // after clear: must rebuild
    CHECK(creates == 2);
}
