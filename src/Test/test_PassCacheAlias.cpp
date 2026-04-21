#include <doctest.h>

#include "VulkanRender/PassCacheAlias.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

using namespace wallpaper::vulkan;

// ─────────────────────────────────────────────────────────────────────────────
// Tests for the pure helpers behind the render-path cache gating added in
// commits 647d421 + 5a99e53 (render-optimizations.md):
//
//   1. ComputeIsLastWriter — alias analysis that decides which cacheable
//      passes may skip-on-re-exec (cached RT bytes only persist if no
//      later pass overwrites the same VkImage).
//
//   2. IsDepthSafeToSkip — invisible-node fast-path guard that prevents
//      stranding a depth image in VK_IMAGE_LAYOUT_UNDEFINED.
//
// These functions are header-only and take ordinary handles/types so the
// test doesn't need a Vulkan device.  Uses uintptr_t as the fake handle
// (VkImage is pointer-sized on 64-bit).
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
using H = std::uintptr_t;
constexpr H NULLH = 0;
constexpr H A     = 0x1111;
constexpr H B     = 0x2222;
constexpr H C     = 0x3333;
} // namespace

TEST_SUITE("PassCacheAlias::ComputeIsLastWriter") {

TEST_CASE("empty sequence returns empty vector") {
    std::vector<H> outs {};
    auto           r = ComputeIsLastWriter<H>(outs);
    CHECK(r.empty());
}

TEST_CASE("single pass writing a handle is its own last writer") {
    std::vector<H> outs { A };
    auto           r = ComputeIsLastWriter<H>(outs);
    REQUIRE(r.size() == 1);
    CHECK(r[0] == true);
}

TEST_CASE("null handles are never last writers") {
    std::vector<H> outs { NULLH, NULLH, NULLH };
    auto           r = ComputeIsLastWriter<H>(outs);
    REQUIRE(r.size() == 3);
    CHECK(r[0] == false);
    CHECK(r[1] == false);
    CHECK(r[2] == false);
}

TEST_CASE("all-unique handles → every pass is a last writer") {
    std::vector<H> outs { A, B, C };
    auto           r = ComputeIsLastWriter<H>(outs);
    REQUIRE(r.size() == 3);
    CHECK(r[0] == true);
    CHECK(r[1] == true);
    CHECK(r[2] == true);
}

TEST_CASE("duplicate handle — only the later position is the last writer") {
    std::vector<H> outs { A, A };
    auto           r = ComputeIsLastWriter<H>(outs);
    REQUIRE(r.size() == 2);
    CHECK(r[0] == false); // pass 0 aliased by pass 1
    CHECK(r[1] == true);
}

TEST_CASE("mixed aliased and unique handles") {
    // Pass order:  [A, B, A, C, B]
    // A's last occurrence is index 2 — pass 0 is aliased-out.
    // B's last is index 4 — pass 1 is aliased-out.
    // C only at index 3.
    std::vector<H> outs { A, B, A, C, B };
    auto           r = ComputeIsLastWriter<H>(outs);
    REQUIRE(r.size() == 5);
    CHECK(r[0] == false);
    CHECK(r[1] == false);
    CHECK(r[2] == true);
    CHECK(r[3] == true);
    CHECK(r[4] == true);
}

TEST_CASE("accumulative render pattern — all passes share one RT") {
    // Mirrors 3body's ~2100 passes writing to _rt_default: only the very
    // last writer of the shared handle gets the true.  The canCache flag
    // for the earlier ones stays false — their cached bytes would be
    // overwritten before the next frame reads.
    std::vector<H> outs(100, A);
    auto           r = ComputeIsLastWriter<H>(outs);
    REQUIRE(r.size() == 100);
    for (size_t i = 0; i < 99; ++i) CHECK(r[i] == false);
    CHECK(r[99] == true);
}

TEST_CASE("nulls interleaved with handles don't affect last-writer decisions") {
    std::vector<H> outs { A, NULLH, A, NULLH };
    auto           r = ComputeIsLastWriter<H>(outs);
    REQUIRE(r.size() == 4);
    CHECK(r[0] == false);
    CHECK(r[1] == false);
    CHECK(r[2] == true);
    CHECK(r[3] == false);
}

TEST_CASE("large sequence with all-unique handles stays O(n)") {
    // Lightweight perf/sanity check: 10k unique handles should complete
    // well under a second even with hash-map reallocation.
    std::vector<H> outs;
    outs.reserve(10000);
    for (size_t i = 0; i < 10000; ++i) outs.push_back(H(0x1000 + i));
    auto r = ComputeIsLastWriter<H>(outs);
    REQUIRE(r.size() == 10000);
    for (size_t i = 0; i < 10000; ++i) CHECK(r[i] == true);
}

} // TEST_SUITE

TEST_SUITE("PassCacheAlias::IsDepthSafeToSkip") {

TEST_CASE("no depth → always safe to skip invisible-node") {
    std::unordered_set<H> inited;
    CHECK(IsDepthSafeToSkip(/*has_depth=*/false, A, inited) == true);
    CHECK(IsDepthSafeToSkip(/*has_depth=*/false, NULLH, inited) == true);
}

TEST_CASE("has depth but null handle → safe (no image to strand)") {
    std::unordered_set<H> inited;
    CHECK(IsDepthSafeToSkip(/*has_depth=*/true, NULLH, inited) == true);
}

TEST_CASE("has depth + handle + image already inited this frame → safe") {
    std::unordered_set<H> inited { A };
    CHECK(IsDepthSafeToSkip(/*has_depth=*/true, A, inited) == true);
}

TEST_CASE("has depth + handle + image NOT yet inited → NOT safe") {
    std::unordered_set<H> inited;
    CHECK(IsDepthSafeToSkip(/*has_depth=*/true, A, inited) == false);
}

TEST_CASE("has depth + handle + different image inited → still NOT safe") {
    // Init tracking is per-image; initing B doesn't help A.
    std::unordered_set<H> inited { B };
    CHECK(IsDepthSafeToSkip(/*has_depth=*/true, A, inited) == false);
}

TEST_CASE("inited set with many entries still O(1) lookup") {
    // Set contains 0x1000..0x1000+999 = 0x13E7.  Pick an in-set handle
    // and an out-of-set handle to confirm both sides.
    std::unordered_set<H> inited;
    for (size_t i = 0; i < 1000; ++i) inited.insert(H(0x1000 + i));
    CHECK(IsDepthSafeToSkip(true, H(0x1200), inited) == true);  // inside
    CHECK(IsDepthSafeToSkip(true, H(0x1500), inited) == false); // outside
    CHECK(IsDepthSafeToSkip(true, H(0x0500), inited) == false); // outside
}

} // TEST_SUITE
