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
using H           = std::uintptr_t;
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
        constexpr H    pp_a       = 0x1111;
        constexpr H    pp_b       = 0x2222;
        constexpr H    bg_pp      = 0x3333;
        constexpr H    rt_default = 0x4444;
        constexpr H    bg_tex     = 0x5555; // external
        constexpr H    body_tex   = 0x6666; // external
        constexpr H    noise_tex  = 0x7777; // external
        constexpr H    shake_mask = 0x8888; // external
        std::vector<H> outs { pp_a, pp_b, pp_a, pp_b, bg_pp, rt_default, rt_default, rt_default };
        std::vector<std::vector<H>> ins {
            { body_tex },         // 0
            { pp_a, shake_mask }, // 1
            { pp_b, shake_mask }, // 2
            { pp_a, shake_mask }, // 3
            { bg_tex },           // 4
            { bg_pp, noise_tex }, // 5
            { body_tex },         // 6 (puppet)
            { pp_b }              // 7 — THE BUG — reads dynamic pp_b
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

// EligibleForOutputCache folds the two carve-outs VulkanRender's cache-gate
// loop applies on top of ComputeIsSafeToSkip's alias verdict: _rt_default is
// never cached, and neither is a terminal-hideable pass (which pipeline it
// draws depends on runtime visibility, so caching it would freeze whichever
// one happened to run through the next toggle).
TEST_SUITE("PassCacheAlias::EligibleForOutputCache") {
    TEST_CASE("_rt_default is never cached, whatever else is true") {
        CHECK_FALSE(EligibleForOutputCache(
            /*is_rt_default=*/true, /*is_hideable=*/false, /*safe_to_skip=*/true));
        CHECK_FALSE(EligibleForOutputCache(
            /*is_rt_default=*/true, /*is_hideable=*/true, /*safe_to_skip=*/true));
        CHECK_FALSE(EligibleForOutputCache(
            /*is_rt_default=*/true, /*is_hideable=*/false, /*safe_to_skip=*/false));
    }
    TEST_CASE("a terminal-hideable pass on a private offscreen RT is still never cached") {
        // The new case: today nothing excludes it, so an unmodified gate
        // would return true here and freeze whichever pipeline last drew.
        CHECK_FALSE(EligibleForOutputCache(
            /*is_rt_default=*/false, /*is_hideable=*/true, /*safe_to_skip=*/true));
    }
    TEST_CASE("an ordinary offscreen/pingpong pass follows the alias verdict") {
        CHECK(EligibleForOutputCache(
            /*is_rt_default=*/false, /*is_hideable=*/false, /*safe_to_skip=*/true));
        CHECK_FALSE(EligibleForOutputCache(
            /*is_rt_default=*/false, /*is_hideable=*/false, /*safe_to_skip=*/false));
    }
}

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

// recordDepthInit() decides whether to issue THIS call's clear-and-transition
// from the same has_depth/depth_image/tracker inputs as IsDepthSafeToSkip,
// negated, plus the tracker-wired precondition (recordDepthInit needs a
// live pointer to insert into, and a null one means compileRenderGraph never
// wired setDepthInitedFrameSet on this pass -- a construction-order bug the
// caller already asserts on in debug builds).
TEST_SUITE("PassCacheAlias::ShouldRecordDepthInit") {
    TEST_CASE("no depth → never records") {
        std::unordered_set<H> inited;
        CHECK(ShouldRecordDepthInit(/*has_depth=*/false, A, &inited) == false);
        CHECK(ShouldRecordDepthInit(/*has_depth=*/false, NULLH, &inited) == false);
    }

    TEST_CASE("has depth but null image → never records") {
        std::unordered_set<H> inited;
        CHECK(ShouldRecordDepthInit(/*has_depth=*/true, NULLH, &inited) == false);
    }

    TEST_CASE("has depth and image but tracker not wired → never records") {
        CHECK(ShouldRecordDepthInit(/*has_depth=*/true, A, (const std::unordered_set<H>*)nullptr) ==
              false);
    }

    TEST_CASE("has depth + image + tracker, image already inited this frame → does not re-record") {
        std::unordered_set<H> inited { A };
        CHECK(ShouldRecordDepthInit(/*has_depth=*/true, A, &inited) == false);
    }

    TEST_CASE("has depth + image + tracker, image NOT yet inited → records") {
        std::unordered_set<H> inited;
        CHECK(ShouldRecordDepthInit(/*has_depth=*/true, A, &inited) == true);
    }

    TEST_CASE("a different image already inited doesn't stop this one recording") {
        std::unordered_set<H> inited { B };
        CHECK(ShouldRecordDepthInit(/*has_depth=*/true, A, &inited) == true);
    }
}

// A pass whose draw depends on runtime visibility must not count as cacheable
// in the alias analysis itself, or a static consumer of its output is judged
// safe to skip and freezes across a toggle.
TEST_SUITE("PassCacheAlias::CacheableForSafetyAnalysis") {
    TEST_CASE("a hideable producer is not cacheable, so its consumer is not safe to skip") {
        // P writes A (hideable, otherwise static); C reads A and writes its own private B.
        std::vector<H>              outs { A, B };
        std::vector<std::vector<H>> ins { {}, { A } };
        std::vector<bool>           cach { CacheableForSafetyAnalysis(true, /*is_hideable=*/true),
                                           CacheableForSafetyAnalysis(true, false) };
        auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
        REQUIRE(r.size() == 2);
        CHECK_FALSE(r[0]);
        CHECK_FALSE(r[1]);
    }
    TEST_CASE("an ordinary cacheable pass keeps its cacheability") {
        CHECK(CacheableForSafetyAnalysis(true, false));
        CHECK_FALSE(CacheableForSafetyAnalysis(false, false));
        CHECK_FALSE(CacheableForSafetyAnalysis(false, true));
    }
}

// A cached pass keeps its bytes only while its inputs are untouched. A producer
// that re-runs this frame (its own uniforms went dirty) must pull its cached
// consumers back in, and a consumer that redraws must do the same for the next.
TEST_SUITE("PassCacheAlias::CachedPassMustRedraw") {
    TEST_CASE("an input rewritten this frame forces a redraw") {
        std::unordered_set<H> rewritten { A };
        std::vector<H>        in { A };
        CHECK(CachedPassMustRedraw<H>(in, rewritten));
    }
    TEST_CASE("untouched inputs leave the cache valid") {
        std::unordered_set<H> rewritten { C };
        std::vector<H>        in { A, B };
        CHECK_FALSE(CachedPassMustRedraw<H>(in, rewritten));
    }
    TEST_CASE("an empty rewritten set never forces a redraw") {
        std::unordered_set<H> rewritten;
        std::vector<H>        in { A, B };
        CHECK_FALSE(CachedPassMustRedraw<H>(in, rewritten));
    }
    TEST_CASE("null and external handles are ignored") {
        std::unordered_set<H> rewritten { A };
        std::vector<H>        in { NULLH, B };
        CHECK_FALSE(CachedPassMustRedraw<H>(in, rewritten));
        std::unordered_set<H> with_null { NULLH };
        CHECK_FALSE(CachedPassMustRedraw<H>(in, with_null));
    }
    TEST_CASE("a pass with no inputs never redraws") {
        std::unordered_set<H> rewritten { A };
        CHECK_FALSE(CachedPassMustRedraw<H>(std::span<const H> {}, rewritten));
    }
    TEST_CASE("redraw propagates down a chain once each consumer records its output") {
        // base(A) -> opacity(B, cached) -> copy(C, cached)
        std::unordered_set<H> rewritten;
        std::vector<H>        opacity_in { A }, copy_in { B };
        rewritten.insert(A); // base re-ran
        bool opacity = CachedPassMustRedraw<H>(opacity_in, rewritten);
        CHECK(opacity);
        if (opacity) rewritten.insert(B);
        CHECK(CachedPassMustRedraw<H>(copy_in, rewritten));
    }
    TEST_CASE("a cache hit does not propagate") {
        std::unordered_set<H> rewritten;
        std::vector<H>        opacity_in { A }, copy_in { B };
        CHECK_FALSE(CachedPassMustRedraw<H>(opacity_in, rewritten));
        CHECK_FALSE(CachedPassMustRedraw<H>(copy_in, rewritten));
    }
}

TEST_SUITE("PassCacheAlias::ComputeIsSafeToSkip non-pass writers") {
    TEST_CASE("a cacheable reader of an image a non-cacheable writer also draws is unsafe") {
        // PrePass / SkyboxPass redraw _rt_default every frame.  Registered as
        // non-cacheable writers they make a cached pass that samples the image
        // unsafe, even when the only other writer of it is that fixed pass.
        // 0: sky -> A (non-cacheable), 1: reader of A -> B (cacheable)
        std::vector<H>              outs { A, B };
        std::vector<std::vector<H>> ins { {}, { A } };
        std::vector<bool>           cach { false, true };
        auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
        REQUIRE(r.size() == 2);
        CHECK_FALSE(r[1]);
    }
    TEST_CASE("an image with a non-cacheable writer and a cacheable one is unsafe for both") {
        std::vector<H>              outs { A, A, B };
        std::vector<std::vector<H>> ins { {}, {}, { A } };
        std::vector<bool>           cach { false, true, true };
        auto                        r = ComputeIsSafeToSkip<H>(outs, ins, cach);
        REQUIRE(r.size() == 3);
        CHECK_FALSE(r[1]);
        CHECK_FALSE(r[2]);
    }
}

namespace
{
// Drives the same per-pass steps CustomShaderPass::execute takes, over several
// frames, so the cache decision under test is the one that runs.
struct SimPass {
    H              out { NULLH };
    std::vector<H> in;
    bool           can_cache { true };
    bool           dirty { false };       // own material went dirty this frame
    bool           visible { true };      // node visibility this frame
    bool           hidden_skip { false }; // execute returns before recording
    bool           cached { false };
    bool           visible_when_cached { true };
    // The output slot's image generation now, and the one this pass cached
    // against; a release-and-recreate of the slot moves the first.
    std::uint32_t  out_gen { 0 };
    std::uint32_t  cached_gen { 0 };
    std::vector<H> backward_in; // filled by SimFrames from pass order
};

struct SimFrames {
    std::unordered_set<H> cur, prev;
    // Returns, per pass, whether it recorded a draw this frame.
    std::vector<bool> run(std::vector<SimPass>& passes) {
        std::vector<H>              outs;
        std::vector<std::vector<H>> ins;
        for (auto& p : passes) {
            outs.push_back(p.out);
            ins.push_back(p.in);
        }
        auto back = ClassifyBackwardInputs<H>(outs, ins);
        for (std::size_t i = 0; i < passes.size(); ++i) passes[i].backward_in = back[i];
        AdvanceRewrittenFrame(cur, prev);
        std::vector<bool> drew;
        for (auto& p : passes) {
            auto d = DecidePassCache<H>(p.can_cache,
                                        p.cached,
                                        p.dirty,
                                        p.visible,
                                        p.visible_when_cached,
                                        p.in,
                                        p.backward_in,
                                        &cur,
                                        &prev,
                                        /*standalone=*/true,
                                        p.out_gen != p.cached_gen);
            if (d.invalidate) p.cached = false;
            if (d.skip) {
                drew.push_back(false);
                continue;
            }
            if (p.hidden_skip) {
                drew.push_back(false);
                continue;
            }
            RecordPassDrawn(&cur, p.out);
            if (p.can_cache) {
                p.cached              = true;
                p.visible_when_cached = p.visible;
                p.cached_gen          = p.out_gen;
            }
            drew.push_back(true);
        }
        return drew;
    }
};
} // namespace

TEST_SUITE("PassCacheAlias::DecidePassCache") {
    using Set = std::unordered_set<H>;
    static PassCacheDecision decide(bool                  can_cache,
                                    bool                  is_cached,
                                    bool                  dirty,
                                    bool                  vis,
                                    bool                  vis_cached,
                                    const std::vector<H>& in,
                                    const std::vector<H>& back,
                                    const Set*            cur,
                                    const Set*            prev,
                                    bool                  standalone       = true,
                                    bool                  generation_moved = false) {
        return DecidePassCache<H>(can_cache,
                                  is_cached,
                                  dirty,
                                  vis,
                                  vis_cached,
                                  in,
                                  back,
                                  cur,
                                  prev,
                                  standalone,
                                  generation_moved);
    }

    TEST_CASE("an uncached or non-cacheable pass is never invalidated or skipped") {
        Set  cur, prev;
        auto d = decide(true, false, true, true, true, { A }, {}, &cur, &prev);
        CHECK_FALSE(d.invalidate);
        CHECK_FALSE(d.skip);
        d = decide(false, true, true, true, true, { A }, {}, &cur, &prev);
        CHECK_FALSE(d.invalidate);
        CHECK_FALSE(d.skip);
    }
    TEST_CASE("a clean cached pass skips only when it is standalone") {
        Set  cur, prev;
        auto d = decide(true, true, false, true, true, { A }, {}, &cur, &prev, true);
        CHECK_FALSE(d.invalidate);
        CHECK(d.skip);
        d = decide(true, true, false, true, true, { A }, {}, &cur, &prev, false);
        CHECK_FALSE(d.invalidate);
        CHECK_FALSE(d.skip);
    }
    TEST_CASE("a replaced output image invalidates a clean cached pass") {
        Set  cur, prev;
        auto d = decide(true,
                        true,
                        false,
                        true,
                        true,
                        { A },
                        {},
                        &cur,
                        &prev,
                        true,
                        /*generation_moved=*/true);
        CHECK(d.invalidate);
        CHECK_FALSE(d.skip);
        d = decide(true, true, false, true, true, { A }, {}, &cur, &prev, true, false);
        CHECK_FALSE(d.invalidate);
        CHECK(d.skip);
    }
    TEST_CASE("own dirty material invalidates") {
        Set  cur, prev;
        auto d = decide(true, true, true, true, true, {}, {}, &cur, &prev);
        CHECK(d.invalidate);
        CHECK_FALSE(d.skip);
    }
    TEST_CASE("a visibility change since the pass was cached invalidates") {
        Set  cur, prev;
        auto d = decide(true, true, false, false, true, {}, {}, &cur, &prev);
        CHECK(d.invalidate);
        CHECK_FALSE(d.skip);
        d = decide(true, true, false, true, false, {}, {}, &cur, &prev);
        CHECK(d.invalidate);
    }
    TEST_CASE("an input drawn this frame invalidates") {
        Set cur { A }, none;
        CHECK(decide(true, true, false, true, true, { A }, {}, &cur, &none).invalidate);
        CHECK_FALSE(decide(true, true, false, true, true, { B }, {}, &cur, &none).invalidate);
    }
    TEST_CASE("last frame's draws count only for backward inputs") {
        // A forward input drawn last frame was already seen last frame; only an
        // input written later in the order is read one frame behind.
        Set none, prev { B };
        CHECK(decide(true, true, false, true, true, { B }, { B }, &none, &prev).invalidate);
        CHECK_FALSE(decide(true, true, false, true, true, { B }, {}, &none, &prev).invalidate);
    }
    TEST_CASE("unwired sets never invalidate") {
        auto d = decide(true, true, false, true, true, { A }, { A }, nullptr, nullptr);
        CHECK_FALSE(d.invalidate);
        CHECK(d.skip);
    }
}

TEST_SUITE("PassCacheAlias::ClassifyBackwardInputs") {
    TEST_CASE("an input written by a later pass is backward") {
        // 0 reads A, 1 writes A
        std::vector<H>              outs { B, A };
        std::vector<std::vector<H>> ins { { A }, {} };
        auto                        r = ClassifyBackwardInputs<H>(outs, ins);
        REQUIRE(r.size() == 2);
        CHECK(r[0] == std::vector<H> { A });
        CHECK(r[1].empty());
    }
    TEST_CASE("an input written by an earlier pass or by nobody is not backward") {
        std::vector<H>              outs { A, B };
        std::vector<std::vector<H>> ins { {}, { A, C, NULLH } };
        auto                        r = ClassifyBackwardInputs<H>(outs, ins);
        CHECK(r[1].empty());
    }
    TEST_CASE("a pass reading its own output reads last frame's") {
        std::vector<H>              outs { A };
        std::vector<std::vector<H>> ins { { A } };
        auto                        r = ClassifyBackwardInputs<H>(outs, ins);
        CHECK(r[0] == std::vector<H> { A });
    }
}

TEST_SUITE("PassCacheAlias::RecordPassDrawn / AdvanceRewrittenFrame") {
    TEST_CASE("records a real output and ignores null or an unwired set") {
        std::unordered_set<H> cur;
        RecordPassDrawn(&cur, A);
        RecordPassDrawn(&cur, NULLH);
        CHECK(cur.size() == 1);
        CHECK(cur.count(A) == 1);
        RecordPassDrawn<H, std::unordered_set<H>>(nullptr, A); // must not crash
    }
    TEST_CASE("advancing keeps the finished frame as prev and empties cur") {
        std::unordered_set<H> cur { A }, prev { B };
        AdvanceRewrittenFrame(cur, prev);
        CHECK(cur.empty());
        CHECK(prev.count(A) == 1);
        CHECK(prev.count(B) == 0);
    }
}

TEST_SUITE("PassCacheAlias::cache chain over frames") {
    // base(A) -> opacity(B, cached) -> copy(C, cached)
    static std::vector<SimPass> chain() {
        SimPass base;
        base.out = A;
        SimPass opacity;
        opacity.out = B;
        opacity.in  = { A };
        SimPass copy;
        copy.out = C;
        copy.in  = { B };
        return { base, opacity, copy };
    }

    TEST_CASE("a dirty base redraws the whole chain once, then everything hits") {
        auto      passes = chain();
        SimFrames sim;
        auto      f1 = sim.run(passes);
        CHECK(f1 == std::vector<bool> { true, true, true });
        auto f2a = sim.run(passes); // clean
        CHECK(f2a == std::vector<bool> { false, false, false });
        passes[0].dirty = true;
        auto f2         = sim.run(passes);
        CHECK(f2 == std::vector<bool> { true, true, true });
        passes[0].dirty = false;
        auto f3         = sim.run(passes);
        CHECK(f3 == std::vector<bool> { false, false, false });
        auto f4 = sim.run(passes);
        CHECK(f4 == std::vector<bool> { false, false, false });
    }

    TEST_CASE("a cached first writer whose slot was released and recreated redraws") {
        // The cached bytes lived in the old image; nothing dirtied the pass or
        // its inputs, so only the generation can tell it.
        auto      passes = chain();
        SimFrames sim;
        sim.run(passes);
        CHECK(sim.run(passes) == std::vector<bool> { false, false, false });
        ++passes[0].out_gen;
        auto f = sim.run(passes);
        CHECK(f == std::vector<bool> { true, true, true });
        CHECK(sim.run(passes) == std::vector<bool> { false, false, false });
    }

    TEST_CASE("a cached pass hidden without recording leaves its consumers cached") {
        // A hidden pass that does not own its target's clear returns before
        // recording: its output keeps the last draw, so readers stay valid.
        auto      passes = chain();
        SimFrames sim;
        sim.run(passes);
        sim.run(passes);
        passes[0].visible     = false;
        passes[0].hidden_skip = true;
        auto f                = sim.run(passes);
        CHECK(f == std::vector<bool> { false, false, false });
        CHECK_FALSE(passes[0].cached); // its own cache was dropped by the change
        auto again = sim.run(passes);
        CHECK(again == std::vector<bool> { false, false, false });
    }

    TEST_CASE("a reader ordered before its writer picks up the change one frame late") {
        // 0: reader of A -> B (cached), 1: writer of A (cached).  The reader
        // samples last frame's A, so a writer re-run in frame F reaches it on F+1.
        SimPass reader;
        reader.out = B;
        reader.in  = { A };
        SimPass writer;
        writer.out = A;
        std::vector<SimPass> passes { reader, writer };
        SimFrames            sim;
        sim.run(passes);
        // The reader's first draw sampled A before the writer ran, so it
        // redraws once on the second frame and then settles.
        auto second = sim.run(passes);
        CHECK(second == std::vector<bool> { true, false });
        auto clean = sim.run(passes);
        CHECK(clean == std::vector<bool> { false, false });
        passes[1].dirty = true;
        auto f          = sim.run(passes); // writer re-runs
        CHECK(f == std::vector<bool> { false, true });
        passes[1].dirty = false;
        auto f1         = sim.run(passes); // reader sees it in prev
        CHECK(f1 == std::vector<bool> { true, false });
        auto f2 = sim.run(passes);
        CHECK(f2 == std::vector<bool> { false, false });
    }

    TEST_CASE("hiding a cached pass redraws it, showing it again redraws once more") {
        auto      passes = chain();
        SimFrames sim;
        sim.run(passes);
        sim.run(passes);
        passes[1].visible = false;
        auto hide         = sim.run(passes);
        CHECK(hide[1]);
        passes[1].visible = true;
        auto show         = sim.run(passes);
        CHECK(show[1]);
        auto settle = sim.run(passes);
        CHECK(settle == std::vector<bool> { false, false, false });
    }
}

#include "VulkanRender/RtResidency.hpp"

// The analyses run on slot keys once a render target's VkImage can change
// under a pass.  Same verdicts as with raw handles; an unbound slot (null
// key) stays invisible.
TEST_SUITE("PassCacheAlias on ResidencyKey") {
    using K = ResidencyKey;
    TEST_CASE("sole-writer and safe-to-skip verdicts match the handle version") {
        const K        rtA = K::ForSlot(1), rtB = K::ForSlot(2), bitmap = K::ForHandle(0x9000);
        std::vector<K> outs { rtA, rtB };
        std::vector<std::vector<K>> ins { { bitmap }, { rtA, K {} } };
        std::vector<bool>           cacheable { true, true };
        auto                        safe = ComputeIsSafeToSkip<K>(outs, ins, cacheable);
        CHECK(safe == std::vector<bool> { true, true });
        auto back = ClassifyBackwardInputs<K>(outs, ins);
        CHECK(back[1].empty());
        ins[0] = { rtB }; // reader of B ahead of B's writer
        back   = ClassifyBackwardInputs<K>(outs, ins);
        CHECK(back[0] == std::vector<K> { rtB });
    }
    TEST_CASE("the null key is never recorded as drawn and never forces a redraw") {
        std::unordered_set<K> drawn;
        RecordPassDrawn<K>(&drawn, K {});
        CHECK(drawn.empty());
        RecordPassDrawn<K>(&drawn, K::ForSlot(3));
        std::vector<K> ins { K {}, K::ForSlot(4) };
        CHECK_FALSE(CachedPassMustRedraw<K>(ins, drawn));
        ins[1] = K::ForSlot(3);
        CHECK(CachedPassMustRedraw<K>(ins, drawn));
    }
}
