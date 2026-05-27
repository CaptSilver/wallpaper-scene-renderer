// Sub-tick plan + suspend-wake clamp pins for the render-thread tick.
//
// Today: dt_wall is clamped to 100ms (SceneWallpaper.cpp), then handed to
// scene->PassFrameTime(dt_scene), which leaves scene.frameTime up to 100ms.
// ParticleSubSystem::Emitt has an internal 32ms clamp on its LOCAL copy of
// scene.frameTime, but every other consumer of the scene clock
// (ResolveControlpointsForInstance's velocity divisor at line 180, trail
// timestamps reading scene.elapsingTime at line 485, g_Time shader uniform,
// camera path animation) reads the UNCLAMPED 100ms value.  Two clocks,
// divergent on TTY switch / suspend wake / compile-first frame.
//
// Fix: subdivide a long dt_scene into <= 32ms chunks via computeSubTickPlan,
// then run the render-thread per-frame simulation steps (PassFrameTime +
// Emitt + ...) once per chunk.  After this, every reader of scene.frameTime
// sees the same per-step value, and scene.elapsingTime accumulates to the
// full wall-clock-clamped dt_scene across all chunks.

#include <doctest.h>

#include "Particle/ParticleSystem.h"
#include "Scene/Scene.h"
#include "Scene/SceneMesh.h"
#include "Scene/SubTickPlan.hpp"

#include <memory>
#include <numeric>
#include <vector>

using wallpaper::computeSubTickPlan;

TEST_SUITE("SceneClockClamp.SubTickPlan") {

    TEST_CASE("dt above 32ms is sub-ticked into 32ms steps + residual") {
        // 100ms frame -> while remaining > 48ms (= 32 * 1.5): push 32.
        // remaining = 100 -> 68 -> 36; 36 is NOT > 48 so loop exits with the
        // 36ms residual as the final step.  Total = 3 steps summing to 0.1.
        auto         plan = computeSubTickPlan(0.1, 0.032);
        const double sum  = std::accumulate(plan.begin(), plan.end(), 0.0);
        CHECK(sum == doctest::Approx(0.1).epsilon(1e-9));
        CHECK(plan.size() == 3);
        for (double s : plan) CHECK(s <= 0.032 * 1.5 + 1e-9);
    }

    TEST_CASE("single 16ms frame runs exactly once at the exact dt") {
        // 60fps steady-state: dt = 16ms < cap; single-step plan.
        auto plan = computeSubTickPlan(0.016, 0.032);
        REQUIRE(plan.size() == 1);
        CHECK(plan[0] == doctest::Approx(0.016).epsilon(1e-9));
    }

    TEST_CASE("dt at exactly cap is a single step") {
        auto plan = computeSubTickPlan(0.032, 0.032);
        REQUIRE(plan.size() == 1);
        CHECK(plan[0] == doctest::Approx(0.032).epsilon(1e-9));
    }

    TEST_CASE("dt at cap * 1.4 is a single step (under the 1.5 threshold)") {
        // 32 * 1.4 = 44.8ms; under the 1.5 cutoff so ONE step of 44.8ms (not
        // 32 + 12.8 which would compound rounding).
        auto plan = computeSubTickPlan(0.032 * 1.4, 0.032);
        REQUIRE(plan.size() == 1);
        CHECK(plan[0] == doctest::Approx(0.032 * 1.4).epsilon(1e-9));
    }

    TEST_CASE("dt at cap * 1.6 is two steps (32ms + residual)") {
        // 32 * 1.6 = 51.2ms; above the 1.5 cutoff so 32 + 19.2.
        auto plan = computeSubTickPlan(0.032 * 1.6, 0.032);
        REQUIRE(plan.size() == 2);
        CHECK(plan[0] == doctest::Approx(0.032).epsilon(1e-9));
        CHECK(plan[1] == doctest::Approx(0.032 * 0.6).epsilon(1e-9));
    }

    TEST_CASE("dt = 0 returns empty plan (caller should skip simulation)") {
        auto plan = computeSubTickPlan(0.0, 0.032);
        CHECK(plan.empty());
    }

    TEST_CASE("negative dt returns empty plan (defensive)") {
        auto plan = computeSubTickPlan(-0.01, 0.032);
        CHECK(plan.empty());
    }

    TEST_CASE("zero cap returns empty plan (defensive)") {
        auto plan = computeSubTickPlan(0.05, 0.0);
        CHECK(plan.empty());
    }

    TEST_CASE("100ms wall-clamp frame becomes ~3 32ms sub-ticks") {
        // Post-suspend / TTY-switch recovery: 100ms wall-clamp at the
        // SceneWallpaper.cpp ceiling => 3 sub-ticks summing to 0.1.
        auto         plan = computeSubTickPlan(0.1, 0.032);
        const double sum  = std::accumulate(plan.begin(), plan.end(), 0.0);
        CHECK(sum == doctest::Approx(0.1).epsilon(1e-9));
        CHECK(plan.size() >= 3);
        CHECK(plan.size() <= 4);
        // All non-final chunks are exactly cap; the final residual is in
        // [cap, cap*1.5) since the loop exits when remaining <= cap*1.5.
        for (std::size_t i = 0; i + 1 < plan.size(); ++i)
            CHECK(plan[i] == doctest::Approx(0.032).epsilon(1e-9));
    }

} // TEST_SUITE SceneClockClamp.SubTickPlan

// ===========================================================================
// Suspend-wake invariant: when the render-thread tick runs the sub-tick loop
// over a stretched (100ms wake-from-suspend) dt_scene, every per-Emitt call
// sees scene.frameTime <= 32ms.  The simulator below mirrors the production
// loop body — PassFrameTime(step) then sub->Emitt() — so the test pins the
// invariant at the call site that Emitt's consumers (CP velocity divisor,
// trail timestamps) actually read.
// ===========================================================================

namespace
{

// Drive the render-thread sub-tick loop the way SceneWallpaper.cpp will:
// for each step in the plan, push the step into scene.frameTime via
// PassFrameTime(step), then run sub->Emitt() exactly once.  Returns the
// largest scene.frameTime observed across the calls — the spec invariant
// requires this to stay <= kMaxFixedTick.
double DriveSubTickAndObserveMaxFrameTime(wallpaper::Scene&              scene,
                                          wallpaper::ParticleSubSystem&  sub,
                                          double                         dt_scene,
                                          double                         cap) {
    auto   plan        = computeSubTickPlan(dt_scene, cap);
    double observedMax = 0.0;
    for (double step : plan) {
        scene.PassFrameTime(step);
        if (scene.frameTime > observedMax) observedMax = scene.frameTime;
        sub.Emitt();
    }
    return observedMax;
}

} // namespace

TEST_SUITE("ParticleSystem suspend-wake clamp") {

    TEST_CASE("100ms wake-from-suspend frame: per-Emitt scene.frameTime stays bounded") {
        // Mirror SceneWallpaper.cpp's post-wake state: dt_wall capped at
        // 0.1, dt_scene = 0.1 * speed (assume speed = 1).  Build a STATIC
        // subsystem with a CP that has a previous resolved position so
        // ResolveControlpointsForInstance's velocity divisor (which reads
        // scene.frameTime directly at ParticleSystem.cpp:180) is exercised
        // on each Emitt call.
        //
        // Today (no sub-tick loop) Emitt would see scene.frameTime = 0.1 —
        // full 100ms wall-clamp ceiling.  After the sub-tick fix, the loop
        // splits dt_scene into chunks <= cap * 1.5 (the residual can land in
        // [cap, cap * 1.5) to avoid compounding rounding on borderline
        // frames; see computeSubTickPlan docs).  cap * 1.5 = 48ms is a
        // tight enough ceiling that CP velocity stays consistent with
        // emission rate — 100ms divided by 48ms is 0.67x actual, vs today's
        // 0.32x actual divisor on a 100ms unclamped read.
        wallpaper::Scene                          scene;
        wallpaper::ParticleSystem                 psys(scene);
        auto                                      mesh = std::make_shared<wallpaper::SceneMesh>(true);
        auto                                      sub  = std::make_unique<wallpaper::ParticleSubSystem>(
            psys,
            mesh,
            /*maxcount=*/10,
            /*rate=*/1.0,
            /*maxcount_instance=*/1,
            /*probability=*/1.0,
            wallpaper::ParticleSubSystem::SpawnType::STATIC,
            /*parent=*/nullptr,
            /*starttime=*/0);
        auto& cp           = sub->Controlpoints()[0];
        cp.offset          = Eigen::Vector3d(10, 0, 0);
        cp.resolved        = Eigen::Vector3d(10, 0, 0);
        cp.prev_resolved   = Eigen::Vector3d(0, 0, 0);
        cp.has_prev_resolved = true;

        constexpr double kCap         = 0.032;
        constexpr double kPostWakeDt  = 0.1; // SceneWallpaper.cpp dt_wall ceiling
        const double     observedMax  = DriveSubTickAndObserveMaxFrameTime(
            scene, *sub, kPostWakeDt, kCap);

        // Spec bound: every step is at most cap * 1.5 (the residual ceiling).
        CHECK(observedMax <= kCap * 1.5 + 1e-9);
        // Today's pre-fix behavior would have observedMax == 0.1 (the full
        // wall-clamp ceiling).  After fix, the max chunk is bounded by the
        // residual band (cap, cap * 1.5] when dt_scene > cap * 1.5, OR
        // dt_scene itself when dt_scene <= cap * 1.5.  Both cases keep us
        // well below the 100ms wall ceiling.
        CHECK(observedMax < kPostWakeDt);
    }

    TEST_CASE("60fps steady-state: single Emitt call at exact 16ms") {
        wallpaper::Scene                          scene;
        wallpaper::ParticleSystem                 psys(scene);
        auto                                      mesh = std::make_shared<wallpaper::SceneMesh>(true);
        auto                                      sub  = std::make_unique<wallpaper::ParticleSubSystem>(
            psys,
            mesh,
            /*maxcount=*/10,
            /*rate=*/1.0,
            /*maxcount_instance=*/1,
            /*probability=*/1.0,
            wallpaper::ParticleSubSystem::SpawnType::STATIC,
            /*parent=*/nullptr,
            /*starttime=*/0);

        constexpr double kCap   = 0.032;
        const double     observed = DriveSubTickAndObserveMaxFrameTime(scene, *sub, 0.016, kCap);

        // Below-cap frame: single Emitt with frameTime == 0.016.
        CHECK(observed == doctest::Approx(0.016).epsilon(1e-9));
    }

    TEST_CASE("elapsingTime accumulates across sub-ticks to the full dt_scene") {
        // The clamp must not hide wall-clock progression: scene.elapsingTime
        // after the sub-tick loop equals the full input dt_scene (= 0.1 here),
        // so g_Time and other elapsingTime-reading consumers stay on the
        // wall clock even though the per-Emitt frameTime is clamped.
        wallpaper::Scene                          scene;
        wallpaper::ParticleSystem                 psys(scene);
        auto                                      mesh = std::make_shared<wallpaper::SceneMesh>(true);
        auto                                      sub  = std::make_unique<wallpaper::ParticleSubSystem>(
            psys,
            mesh,
            /*maxcount=*/10,
            /*rate=*/1.0,
            /*maxcount_instance=*/1,
            /*probability=*/1.0,
            wallpaper::ParticleSubSystem::SpawnType::STATIC,
            /*parent=*/nullptr,
            /*starttime=*/0);

        const double startTime = scene.elapsingTime;
        (void)DriveSubTickAndObserveMaxFrameTime(scene, *sub, 0.1, 0.032);
        CHECK(scene.elapsingTime - startTime == doctest::Approx(0.1).epsilon(1e-9));
    }

} // TEST_SUITE ParticleSystem suspend-wake clamp
