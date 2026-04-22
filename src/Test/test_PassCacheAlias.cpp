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

TEST_SUITE("PassCacheAlias::ComputeIsSoleWriter") {

TEST_CASE("empty sequence returns empty vector") {
    std::vector<H> outs {};
    auto           r = ComputeIsSoleWriter<H>(outs);
    CHECK(r.empty());
}

TEST_CASE("single pass writing a handle is its own sole writer") {
    std::vector<H> outs { A };
    auto           r = ComputeIsSoleWriter<H>(outs);
    REQUIRE(r.size() == 1);
    CHECK(r[0] == true);
}

TEST_CASE("nulls are never sole writers") {
    std::vector<H> outs { NULLH, NULLH };
    auto           r = ComputeIsSoleWriter<H>(outs);
    REQUIRE(r.size() == 2);
    CHECK(r[0] == false);
    CHECK(r[1] == false);
}

TEST_CASE("all-unique handles → every pass is a sole writer") {
    std::vector<H> outs { A, B, C };
    auto           r = ComputeIsSoleWriter<H>(outs);
    REQUIRE(r.size() == 3);
    CHECK(r[0] == true);
    CHECK(r[1] == true);
    CHECK(r[2] == true);
}

TEST_CASE("duplicate handle — NEITHER position is a sole writer") {
    // Unlike last-writer where the later wins, sole-writer requires
    // exactly one.  Two writes to A disqualifies both.
    std::vector<H> outs { A, A };
    auto           r = ComputeIsSoleWriter<H>(outs);
    REQUIRE(r.size() == 2);
    CHECK(r[0] == false);
    CHECK(r[1] == false);
}

TEST_CASE("accumulative render pattern — NONE of the aliased writers is sole") {
    // Key semantic difference from last-writer: when 100 passes write
    // the same RT, last-writer flags the final one true but sole-writer
    // flags ALL false.  This is the invariant the pass cache needs:
    // skipping any of them would leave its cached bytes overwritten by
    // the others on the next frame.
    std::vector<H> outs(100, A);
    auto           r = ComputeIsSoleWriter<H>(outs);
    REQUIRE(r.size() == 100);
    for (size_t i = 0; i < 100; ++i) CHECK(r[i] == false);
}

TEST_CASE("mixed aliased and unique handles") {
    // [A, B, A, C, B] — A and B each appear twice, C once.
    std::vector<H> outs { A, B, A, C, B };
    auto           r = ComputeIsSoleWriter<H>(outs);
    REQUIRE(r.size() == 5);
    CHECK(r[0] == false); // A has 2 writers
    CHECK(r[1] == false); // B has 2 writers
    CHECK(r[2] == false); // A has 2 writers
    CHECK(r[3] == true);  // C sole writer
    CHECK(r[4] == false); // B has 2 writers
}

TEST_CASE("large sequence with all-unique handles stays O(n)") {
    std::vector<H> outs;
    outs.reserve(10000);
    for (size_t i = 0; i < 10000; ++i) outs.push_back(H(0x1000 + i));
    auto r = ComputeIsSoleWriter<H>(outs);
    REQUIRE(r.size() == 10000);
    for (size_t i = 0; i < 10000; ++i) CHECK(r[i] == true);
}

} // TEST_SUITE

TEST_SUITE("PassCacheAlias::ComputeIsSafeToSkip") {

TEST_CASE("empty → empty") {
    std::vector<H>              outs {};
    std::vector<std::vector<H>> ins {};
    std::vector<bool>           cach {};
    auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    CHECK(r.empty());
}

TEST_CASE("sole-writer + cacheable + static inputs → safe") {
    // Single pass writing A, reading an external texture B (not in
    // outputs).  Cacheable.  Safe to skip.
    std::vector<H>              outs { A };
    std::vector<std::vector<H>> ins { { B } };
    std::vector<bool>           cach { true };
    auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    REQUIRE(r.size() == 1);
    CHECK(r[0] == true);
}

TEST_CASE("not cacheable → not safe even if sole writer with static inputs") {
    std::vector<H>              outs { A };
    std::vector<std::vector<H>> ins { { B } };
    std::vector<bool>           cach { false };
    auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    REQUIRE(r.size() == 1);
    CHECK(r[0] == false);
}

TEST_CASE("aliased output → NOT safe (the existing bug we fix)") {
    // Two passes write A.  Even if both are cacheable with static
    // inputs, neither can be cached — the other writer would overwrite
    // the cached bytes each frame.  ComputeIsLastWriter would have
    // marked the second one true, causing the 3018516781 regression.
    std::vector<H>              outs { A, A };
    std::vector<std::vector<H>> ins { {}, {} };
    std::vector<bool>           cach { true, true };
    auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    REQUIRE(r.size() == 2);
    CHECK(r[0] == false);
    CHECK(r[1] == false);
}

TEST_CASE("cacheable pass reading a dynamic (non-cacheable) RT → not safe") {
    // Pass 0 writes B, not cacheable (time-based shader).
    // Pass 1 writes A (sole writer), cacheable, reads B.
    // Pass 1's output depends on B's frame-N value; skipping after
    // frame 0 would freeze the output at frame-0 data.  This is the
    // exact 2B wallpaper pattern: static-shader compositor reading a
    // dynamic shake pingpong RT.
    std::vector<H>              outs { B, A };
    std::vector<std::vector<H>> ins { {}, { B } };
    std::vector<bool>           cach { false, true };
    auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    REQUIRE(r.size() == 2);
    CHECK(r[0] == false);
    CHECK(r[1] == false); // unsafe — input B churns each frame
}

TEST_CASE("safe input chain — cacheable pass reads cacheable-sole RT") {
    // Pass 0 writes B (sole, cacheable, static inputs) → safe.
    // Pass 1 writes A (sole, cacheable), reads B — B is itself safe,
    // so pass 1's inputs are effectively stable → safe.
    std::vector<H>              outs { B, A };
    std::vector<std::vector<H>> ins { {}, { B } };
    std::vector<bool>           cach { true, true };
    auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    REQUIRE(r.size() == 2);
    CHECK(r[0] == true);
    CHECK(r[1] == true);
}

TEST_CASE("transitive propagation — unsafe flag reaches length-3 chain") {
    // Pass 0 writes C, not cacheable (dynamic source).
    // Pass 1 writes B, cacheable, reads C → becomes unsafe.
    // Pass 2 writes A, cacheable, reads B → must also become unsafe
    // via fixpoint propagation.
    std::vector<H>              outs { C, B, A };
    std::vector<std::vector<H>> ins { {}, { C }, { B } };
    std::vector<bool>           cach { false, true, true };
    auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    REQUIRE(r.size() == 3);
    CHECK(r[0] == false);
    CHECK(r[1] == false);
    CHECK(r[2] == false);
}

TEST_CASE("2B wallpaper 3018516781 regression scenario") {
    // Reproduces the exact render graph shape that caused the missing
    // character regression.  Simplified to just the relevant passes:
    //   0: body base texture → pingpong_a (static, cacheable)
    //   1: shake            → pingpong_b (time-based, not cacheable)
    //   2: waterwaves       → pingpong_a (time-based, not cacheable)
    //   3: shake            → pingpong_b (time-based, not cacheable)
    //   4: bg base          → bg_pingpong (static, cacheable, sole)
    //   5: pulse bg         → _rt_default (time-based, not cacheable)
    //   6: hand puppet      → _rt_default (dyn vertex, not cacheable)
    //   7: 2B composite     → _rt_default (static shader, cacheable,
    //                                       reads pingpong_b)
    //
    // Expected safe-to-skip: ONLY pass 4 (bg_pingpong, sole + cacheable,
    // external static input).  Pass 7 must NOT be safe — that's the bug.
    constexpr H pp_a         = 0x1111;
    constexpr H pp_b         = 0x2222;
    constexpr H bg_pp        = 0x3333;
    constexpr H rt_default   = 0x4444;
    constexpr H bg_tex       = 0x5555; // external
    constexpr H body_tex     = 0x6666; // external
    constexpr H noise_tex    = 0x7777; // external
    constexpr H shake_mask   = 0x8888; // external
    std::vector<H>              outs { pp_a, pp_b, pp_a, pp_b, bg_pp, rt_default, rt_default,
                                       rt_default };
    std::vector<std::vector<H>> ins {
        { body_tex },             // 0
        { pp_a, shake_mask },     // 1
        { pp_b, shake_mask },     // 2
        { pp_a, shake_mask },     // 3
        { bg_tex },               // 4
        { bg_pp, noise_tex },     // 5
        { body_tex },             // 6 (puppet)
        { pp_b }                  // 7 — THE BUG — reads dynamic pp_b
    };
    std::vector<bool> cach { true, false, false, false, true, false, false, true };
    auto              r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    REQUIRE(r.size() == 8);
    CHECK(r[0] == false); // aliased: pass 2 also writes pp_a
    CHECK(r[1] == false); // not cacheable
    CHECK(r[2] == false); // not cacheable
    CHECK(r[3] == false); // not cacheable
    CHECK(r[4] == true);  // bg base — sole writer, cacheable, static input
    CHECK(r[5] == false); // not cacheable
    CHECK(r[6] == false); // not cacheable
    CHECK(r[7] == false); // THE FIX — aliased output + dynamic input pp_b
}

TEST_CASE("fixpoint converges under mutual RT references") {
    // Three passes each write their own sole RT and all are cacheable,
    // but pass 0's input is written by pass 2 → creates a ring:
    // 0 reads 2, 1 reads 0, 2 reads 1.  No pass has a static input
    // outside the ring.  One pass being unsafe tears the whole ring.
    // But with ALL cacheable + sole-writer + all inputs in-ring → all
    // should stay safe (the ring is self-contained).
    std::vector<H>              outs { A, B, C };
    std::vector<std::vector<H>> ins { { C }, { A }, { B } };
    std::vector<bool>           cach { true, true, true };
    auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    REQUIRE(r.size() == 3);
    CHECK(r[0] == true);
    CHECK(r[1] == true);
    CHECK(r[2] == true);
}

TEST_CASE("null input handles are ignored") {
    std::vector<H>              outs { A };
    std::vector<std::vector<H>> ins { { NULLH, NULLH } };
    std::vector<bool>           cach { true };
    auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    REQUIRE(r.size() == 1);
    CHECK(r[0] == true);
}

TEST_CASE("short is_cacheable vector treats missing entries as false") {
    std::vector<H>              outs { A, B };
    std::vector<std::vector<H>> ins { {}, {} };
    std::vector<bool>           cach { true }; // only 1 entry for 2 passes
    auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
    REQUIRE(r.size() == 2);
    CHECK(r[0] == true);
    CHECK(r[1] == false); // missing cacheable entry → unsafe
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
