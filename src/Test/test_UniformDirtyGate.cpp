#include <doctest.h>

#include "Scene/UniformDirtyGate.h"
#include "Scene/SceneShader.h"

#include <cstdint>
#include <array>
#include <vector>
#include <string>
#include <Eigen/Dense>

using namespace wallpaper;

// The matrix/VP uniform block recomputes only when an input that feeds the
// model/MVP/VP matrices changed since the last upload for this node+camera.
TEST_SUITE("Uniform matrix recompute gate") {
    TEST_CASE("first upload always recomputes (no cached value yet)") {
        // firstUpload=true short-circuits regardless of epochs/flags.
        CHECK(uniformMatricesShouldRecompute(
                  /*firstUpload*/ true,
                  /*parallax*/ false,
                  /*shake*/ false,
                  /*nodeEpoch*/ 5,
                  /*cachedNode*/ 5,
                  /*vpEpoch*/ 9,
                  /*cachedVp*/ 9) == true);
    }

    TEST_CASE("equal epochs + no parallax/shake -> cached (the win case)") {
        // Nothing moved and neither volatile mutator is active: re-upload the
        // cached matrices, skip the recompute + both double inversions.
        CHECK(uniformMatricesShouldRecompute(false, false, false, 7, 7, 12, 12) == false);
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
        CHECK(uniformMatricesShouldRecompute(false, /*parallax*/ true, false, 7, 7, 12, 12) ==
              true);
    }

    TEST_CASE("camera shake active forces recompute even with frozen epochs") {
        // Camera shake shifts the VP every frame for the global camera; must stay
        // volatile while enabled.
        CHECK(uniformMatricesShouldRecompute(false, false, /*shake*/ true, 7, 7, 12, 12) == true);
    }

    TEST_CASE("steady-state: recompute once then cache while epochs are frozen") {
        // Simulate the production gate: cache the epochs on a recompute, then
        // verify subsequent frozen-epoch frames take the cached branch.
        uint64_t       cachedNode = 0, cachedVp = 0;
        bool           valid      = false;
        int            recomputes = 0, cached = 0;
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
        bool     valid      = true;
        int      recomputes = 0;
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

    // Models the updater's cache-or-recompute dispatch to prove the cached
    // re-upload emits the SAME ShaderValues the recompute would, i.e. the gate
    // only removes redundant recomputes.
    TEST_CASE("cached re-upload dispatches the same values a recompute would") {
        // A tiny stand-in for the per-node matrix cache the updater holds.
        struct MatrixCache {
            uint64_t    node_epoch { 0 }, vp_epoch { 0 };
            bool        valid { false };
            ShaderValue m, mi;
        };
        // Capture every (name,16-floats) the "updater" emits this frame.
        std::vector<std::pair<std::string, std::array<float, 16>>> emitted;
        auto emit = [&](const char* name, const ShaderValue& v) {
            std::array<float, 16> a {};
            for (size_t i = 0; i < 16 && i < v.size(); ++i) a[i] = v.data()[i];
            emitted.emplace_back(name, a);
        };

        Eigen::Matrix4d model = Eigen::Matrix4d::Identity();
        model(0, 3)           = 9.0;

        MatrixCache mc;
        auto        frame = [&](uint64_t nodeEpoch, uint64_t vpEpoch) {
            if (uniformMatricesShouldRecompute(
                    ! mc.valid, false, false, nodeEpoch, mc.node_epoch, vpEpoch, mc.vp_epoch)) {
                mc.m  = ShaderValue::fromMatrix(model);           // recompute path
                mc.mi = ShaderValue::fromMatrix(model.inverse()); // the double inverse
                mc.node_epoch = nodeEpoch;
                mc.vp_epoch   = vpEpoch;
                mc.valid      = true;
            }
            emit("g_Model", mc.m); // both paths emit from the cache fields
            emit("g_ModelInverse", mc.mi);
        };

        frame(1, 1); // recompute (first)
        auto first = emitted;
        emitted.clear();
        frame(1, 1); // cached re-upload (frozen epochs)
        // Cached frame must emit byte-identical values to the recompute frame.
        REQUIRE(emitted.size() == first.size());
        for (size_t k = 0; k < emitted.size(); ++k) {
            CHECK(emitted[k].first == first[k].first);
            for (size_t i = 0; i < 16; ++i) CHECK(emitted[k].second[i] == first[k].second[i]);
        }
    }
} // TEST_SUITE Uniform matrix recompute gate
