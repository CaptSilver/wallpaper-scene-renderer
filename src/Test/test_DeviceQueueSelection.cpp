#include <doctest.h>

#include "Vulkan/Device.hpp"

using wallpaper::vulkan::queue_selection::FindUnifiedFamily;

TEST_SUITE_BEGIN("DeviceQueueSelection");

TEST_CASE("FindUnifiedFamily: unified GPU (RADV/NV/ANV) - family 0 in both") {
    // Most desktop GPUs expose a single graphics+present family at index 0.
    CHECK(FindUnifiedFamily({ 0 }, { 0 }) == 0u);
}

TEST_CASE("FindUnifiedFamily: graphics-front presents - preferred") {
    // When the first graphics family also presents, we use it (single queue).
    CHECK(FindUnifiedFamily({ 0, 1 }, { 0, 2 }) == 0u);
}

TEST_CASE("FindUnifiedFamily: only a later graphics family presents") {
    // Pathological GPU where graphic_indexs.front() doesn't support present
    // but a later graphics family does.  The old code compared fronts only
    // and split into 2 queues; the new code finds the overlap.
    CHECK(FindUnifiedFamily({ 0, 1, 2 }, { 1, 3 }) == 1u);
}

TEST_CASE("FindUnifiedFamily: only a later present family overlaps") {
    CHECK(FindUnifiedFamily({ 2 }, { 0, 1, 2, 3 }) == 2u);
}

TEST_CASE("FindUnifiedFamily: truly split - no overlap returns UINT32_MAX") {
    // Hypothetical compute-only graphics + present-only families.  Real
    // desktop hardware doesn't seem to expose this configuration, but the
    // function must report it cleanly so the caller can log + fall back.
    CHECK(FindUnifiedFamily({ 0 }, { 1 }) == UINT32_MAX);
    CHECK(FindUnifiedFamily({ 0, 2 }, { 1, 3 }) == UINT32_MAX);
}

TEST_CASE("FindUnifiedFamily: empty graphics list returns UINT32_MAX") {
    CHECK(FindUnifiedFamily({}, { 0 }) == UINT32_MAX);
}

TEST_CASE("FindUnifiedFamily: empty present list returns UINT32_MAX") {
    CHECK(FindUnifiedFamily({ 0 }, {}) == UINT32_MAX);
}

TEST_CASE("FindUnifiedFamily: both empty returns UINT32_MAX") {
    CHECK(FindUnifiedFamily({}, {}) == UINT32_MAX);
}

TEST_SUITE_END();
