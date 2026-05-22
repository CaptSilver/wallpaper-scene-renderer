#include <doctest.h>

#include "Scene/UniformDirtyGate.h"

#include <cstdint>

using namespace wallpaper;

// The matrix/VP uniform block recomputes only when an input that feeds the
// model/MVP/VP matrices changed since the last upload for this node+camera.
TEST_SUITE("Uniform matrix recompute gate") {
    TEST_CASE("first upload always recomputes (no cached value yet)") {
        // firstUpload=true short-circuits regardless of epochs/flags.
        CHECK(uniformMatricesShouldRecompute(
                  /*firstUpload*/ true, /*parallax*/ false, /*shake*/ false,
                  /*nodeEpoch*/ 5, /*cachedNode*/ 5, /*vpEpoch*/ 9, /*cachedVp*/ 9) == true);
    }

    TEST_CASE("equal epochs + no parallax/shake -> cached (the win case)") {
        // Nothing moved and neither volatile mutator is active: re-upload the
        // cached matrices, skip the recompute + both double inversions.
        CHECK(uniformMatricesShouldRecompute(
                  false, false, false, 7, 7, 12, 12) == false);
    }

    TEST_CASE("advanced node transform epoch -> recompute") {
        CHECK(uniformMatricesShouldRecompute(
                  false, false, false, /*node*/ 8, /*cachedNode*/ 7, 12, 12) == true);
    }

    TEST_CASE("advanced camera VP epoch -> recompute") {
        CHECK(uniformMatricesShouldRecompute(
                  false, false, false, 7, 7, /*vp*/ 13, /*cachedVp*/ 12) == true);
    }

    TEST_CASE("parallax active forces recompute even with frozen epochs") {
        // Parallax shifts the model matrix from live mouse input every frame; it
        // must NEVER be served from the static cache while enabled.
        CHECK(uniformMatricesShouldRecompute(
                  false, /*parallax*/ true, false, 7, 7, 12, 12) == true);
    }

    TEST_CASE("camera shake active forces recompute even with frozen epochs") {
        // Camera shake shifts the VP every frame for the global camera; must stay
        // volatile while enabled.
        CHECK(uniformMatricesShouldRecompute(
                  false, false, /*shake*/ true, 7, 7, 12, 12) == true);
    }

    TEST_CASE("steady-state: recompute once then cache while epochs are frozen") {
        // Simulate the production gate: cache the epochs on a recompute, then
        // verify subsequent frozen-epoch frames take the cached branch.
        uint64_t cachedNode = 0, cachedVp = 0;
        bool     valid       = false;
        int      recomputes  = 0, cached = 0;
        const uint64_t nodeEpoch = 4, vpEpoch = 6; // frozen across 5 frames
        for (int frame = 0; frame < 5; ++frame) {
            bool rc = uniformMatricesShouldRecompute(
                /*firstUpload*/ ! valid, false, false, nodeEpoch, cachedNode, vpEpoch, cachedVp);
            if (rc) {
                ++recomputes;
                cachedNode = nodeEpoch;
                cachedVp   = vpEpoch;
                valid      = true;
            } else {
                ++cached;
            }
        }
        CHECK(recomputes == 1); // only the first frame recomputes
        CHECK(cached == 4);     // the rest re-upload the cache
    }

    TEST_CASE("a move mid-stream re-triggers exactly one recompute") {
        uint64_t cachedNode = 3, cachedVp = 6;
        bool     valid       = true;
        int      recomputes  = 0;
        // Frame A: frozen -> cached.
        if (uniformMatricesShouldRecompute(! valid, false, false, 3, cachedNode, 6, cachedVp)) {
            ++recomputes;
            cachedNode = 3;
            cachedVp   = 6;
        }
        // Frame B: node moved (epoch 3 -> 4) -> recompute, re-cache.
        if (uniformMatricesShouldRecompute(! valid, false, false, 4, cachedNode, 6, cachedVp)) {
            ++recomputes;
            cachedNode = 4;
            cachedVp   = 6;
        }
        // Frame C: frozen again -> cached.
        if (uniformMatricesShouldRecompute(! valid, false, false, 4, cachedNode, 6, cachedVp)) {
            ++recomputes;
            cachedNode = 4;
            cachedVp   = 6;
        }
        CHECK(recomputes == 1); // only frame B
    }
} // TEST_SUITE Uniform matrix recompute gate
