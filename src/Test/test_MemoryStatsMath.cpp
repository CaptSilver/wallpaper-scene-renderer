#include <doctest.h>

#include "Vulkan/MemoryStatsMath.hpp"

using namespace wallpaper::vulkan;

TEST_SUITE("MemoryStatsMath") {
    TEST_CASE("bytes convert to binary megabytes") {
        CHECK(BytesToMb(0) == doctest::Approx(0.0));
        CHECK(BytesToMb(1024ull * 1024ull) == doctest::Approx(1.0));
        CHECK(BytesToMb(3ull << 29) == doctest::Approx(1536.0));
    }

    TEST_CASE("the block total is live bytes plus the free space inside the blocks") {
        const auto m = MemoryStatsMbOf(5ull << 20, 3ull << 20, 2ull << 20, 4ull << 20);
        CHECK(m.used == doctest::Approx(5.0));
        CHECK(m.unused == doctest::Approx(3.0));
        CHECK(m.block_total == doctest::Approx(8.0));
        CHECK(m.unused_range_max == doctest::Approx(2.0));
        CHECK(m.alloc_max == doctest::Approx(4.0));
    }

    TEST_CASE("sizes above 4 GiB do not wrap") {
        const auto m = MemoryStatsMbOf(6ull << 30, 3ull << 30, 0, 0);
        CHECK(m.block_total == doctest::Approx(9.0 * 1024.0));
    }
}
