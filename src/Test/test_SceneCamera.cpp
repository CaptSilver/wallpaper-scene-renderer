#include <doctest.h>

#include "Scene/SceneCamera.h"

#include <cmath>

using namespace wallpaper;
using namespace Eigen;

namespace
{

// Helper: create a perspective camera (like 3D wallpapers)
SceneCamera makePerspCamera() { return SceneCamera(16.0f / 9.0f, 0.01f, 1000.0f, 50.0f); }

// Helper: build a camera path from eye start/end with shared center and up
CameraPath makePath(Vector3d eye0, Vector3d eye1, Vector3d center, Vector3d up, double duration) {
    CameraPath cp;
    cp.duration = duration;
    CameraKeyframe kf0, kf1;
    kf0.eye    = eye0;
    kf0.center = center;
    kf0.up     = up;
    kf1.eye    = eye1;
    kf1.center = center;
    kf1.up     = up;
    cp.keyframes = { kf0, kf1 };
    return cp;
}

// Overload with default center/up
CameraPath makePath(Vector3d eye0, Vector3d eye1, double duration) {
    return makePath(eye0, eye1, Vector3d::Zero(), Vector3d::UnitY(), duration);
}

// Smoothstep reference for assertions
double smoothstep(double t) { return t * t * (3.0 - 2.0 * t); }

} // namespace

// ===========================================================================
// Path Interpolation (no fade)
// ===========================================================================

TEST_SUITE("SceneCamera_PathInterpolation") {

TEST_CASE("empty paths: AdvanceTime is a no-op") {
    auto cam = makePerspCamera();
    Vector3d origEye = cam.GetEye();
    cam.AdvanceTime(1.0);
    CHECK(cam.GetEye().isApprox(origEye));
}

TEST_CASE("LoadPaths sets camera to first keyframe") {
    auto cam = makePerspCamera();
    Vector3d eye0(1, 2, 3);
    std::vector<CameraPath> paths = { makePath(eye0, Vector3d(4, 5, 6), 10.0) };
    cam.LoadPaths(std::move(paths));
    CHECK(cam.GetEye().isApprox(eye0));
}

TEST_CASE("linear interpolation at midpoint") {
    auto cam = makePerspCamera();
    Vector3d eye0(0, 0, 0), eye1(10, 0, 0);
    cam.LoadPaths({ makePath(eye0, eye1, 10.0) });

    cam.AdvanceTime(5.0); // t = 0.5
    CHECK(cam.GetEye().x() == doctest::Approx(5.0));
    CHECK(cam.GetEye().y() == doctest::Approx(0.0));
    CHECK(cam.GetEye().z() == doctest::Approx(0.0));
}

TEST_CASE("linear interpolation at endpoints") {
    auto cam = makePerspCamera();
    Vector3d eye0(0, 0, 0), eye1(10, 20, 30);
    cam.LoadPaths({ makePath(eye0, eye1, 10.0) });

    // At t=0 (after LoadPaths, before any advance)
    CHECK(cam.GetEye().isApprox(eye0));

    // At t=0.25
    cam.AdvanceTime(2.5);
    CHECK(cam.GetEye().x() == doctest::Approx(2.5));
    CHECK(cam.GetEye().y() == doctest::Approx(5.0));
    CHECK(cam.GetEye().z() == doctest::Approx(7.5));
}

TEST_CASE("center and up are interpolated") {
    auto cam = makePerspCamera();
    CameraPath cp;
    cp.duration = 10.0;
    CameraKeyframe kf0, kf1;
    kf0.eye    = Vector3d(0, 0, 5);
    kf0.center = Vector3d(0, 0, 0);
    kf0.up     = Vector3d(0, 1, 0);
    kf1.eye    = Vector3d(0, 0, 5);
    kf1.center = Vector3d(10, 0, 0);
    kf1.up     = Vector3d(0, 1, 0);
    cp.keyframes = { kf0, kf1 };
    cam.LoadPaths({ cp });

    cam.AdvanceTime(5.0); // t = 0.5
    CHECK(cam.GetCenter().x() == doctest::Approx(5.0));
    CHECK(cam.GetCenter().y() == doctest::Approx(0.0));
    CHECK(cam.GetCenter().z() == doctest::Approx(0.0));
}

TEST_CASE("up vector is normalized after interpolation") {
    auto cam = makePerspCamera();
    CameraPath cp;
    cp.duration = 10.0;
    CameraKeyframe kf0, kf1;
    kf0.eye = kf1.eye = Vector3d(0, 0, 5);
    kf0.center = kf1.center = Vector3d::Zero();
    kf0.up     = Vector3d(0, 1, 0);
    kf1.up     = Vector3d(1, 0, 0);
    cp.keyframes = { kf0, kf1 };
    cam.LoadPaths({ cp });

    cam.AdvanceTime(5.0); // t = 0.5
    CHECK(cam.GetUp().norm() == doctest::Approx(1.0));
}

} // TEST_SUITE

// ===========================================================================
// Path Cycling
// ===========================================================================

TEST_SUITE("SceneCamera_PathCycling") {

TEST_CASE("cycles to next path after duration") {
    auto cam = makePerspCamera();
    Vector3d eye0a(0, 0, 0), eye1a(10, 0, 0);
    Vector3d eye0b(0, 5, 0), eye1b(0, 5, 10);
    cam.LoadPaths({ makePath(eye0a, eye1a, 10.0), makePath(eye0b, eye1b, 10.0) });

    // Advance to end of path 0 + small into path 1
    cam.AdvanceTime(10.5);
    // Should be on path 1 at t=0.05 → eye ≈ (0, 5, 0.5)
    CHECK(cam.GetEye().x() == doctest::Approx(0.0));
    CHECK(cam.GetEye().y() == doctest::Approx(5.0));
    CHECK(cam.GetEye().z() == doctest::Approx(0.5));
}

TEST_CASE("wraps around to first path") {
    auto cam = makePerspCamera();
    Vector3d eye0a(0, 0, 0), eye1a(10, 0, 0);
    Vector3d eye0b(0, 5, 0), eye1b(0, 5, 10);
    cam.LoadPaths({ makePath(eye0a, eye1a, 10.0), makePath(eye0b, eye1b, 10.0) });

    // Advance through both paths + into first path again
    cam.AdvanceTime(10.0);
    cam.AdvanceTime(10.0);
    // Back on path 0 at t=0
    CHECK(cam.GetEye().x() == doctest::Approx(0.0));
}

TEST_CASE("single path loops to itself") {
    auto cam = makePerspCamera();
    cam.LoadPaths({ makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 5.0) });

    cam.AdvanceTime(5.5); // past duration, wraps to same path at t=0.5/5.0
    CHECK(cam.GetEye().x() == doctest::Approx(1.0));
}

TEST_CASE("very large dt clamps to path start") {
    auto cam = makePerspCamera();
    cam.LoadPaths({ makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 5.0) });

    cam.AdvanceTime(999.0); // huge dt, pathTime clamped to 0
    CHECK(cam.GetEye().x() == doctest::Approx(0.0));
}

} // TEST_SUITE

// ===========================================================================
// Camera Fade
// ===========================================================================

TEST_SUITE("SceneCamera_Fade") {

TEST_CASE("disabled by default") {
    auto cam = makePerspCamera();
    CHECK_FALSE(cam.IsFadeEnabled());
}

TEST_CASE("can be enabled") {
    auto cam = makePerspCamera();
    cam.SetFadeEnabled(true);
    CHECK(cam.IsFadeEnabled());
}

TEST_CASE("no fade: hard cut at path boundary") {
    auto cam = makePerspCamera();
    // Path 0: eye (0,0,0) → (10,0,0) over 10s
    // Path 1: eye (0,50,0) → (0,50,100) over 10s
    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 10.0),
        makePath(Vector3d(0, 50, 0), Vector3d(0, 50, 100), 10.0),
    });

    // Advance to just past boundary
    cam.AdvanceTime(10.1);
    // Without fade, camera snaps to path 1 interpolation at t=0.01
    CHECK(cam.GetEye().y() == doctest::Approx(50.0));
}

TEST_CASE("fade: camera blends at path boundary") {
    auto cam = makePerspCamera();
    cam.SetFadeEnabled(true);

    // Path 0: eye X goes 0→10 over 10s
    // Path 1: eye Y=50, starts at (0,50,0)
    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 10.0),
        makePath(Vector3d(0, 50, 0), Vector3d(0, 50, 100), 10.0),
    });

    // Advance to near end of path 0 (m_eye ≈ (9.9, 0, 0))
    cam.AdvanceTime(9.9);
    Vector3d eyeBeforeTransition = cam.GetEye();
    CHECK(eyeBeforeTransition.x() == doctest::Approx(9.9));

    // Cross the boundary — fade starts
    cam.AdvanceTime(0.2); // now on path 1 at t=0.1/10.0
    // With fade, camera should NOT be at (0, 50, 1.0) — it should be near old position
    // fadeTime=0.2, alpha=smoothstep(0.2/1.5) ≈ smoothstep(0.133) ≈ 0.025
    double alpha = smoothstep(0.2 / 1.5);
    // Path 1 target: eye = (0, 50, 0) + 0.01*(0, 50, 100)-(0,50,0)) = (0, 50, 1.0)
    // Blended Y ≈ 0 + alpha * 50
    CHECK(cam.GetEye().y() == doctest::Approx(alpha * 50.0).epsilon(0.5));
    // Y should be much less than 50 (still near 0)
    CHECK(cam.GetEye().y() < 10.0);
}

TEST_CASE("fade completes after duration") {
    auto cam = makePerspCamera();
    cam.SetFadeEnabled(true);

    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 10.0),
        makePath(Vector3d(0, 50, 0), Vector3d(0, 50, 100), 10.0),
    });

    // Advance to end of path 0
    cam.AdvanceTime(9.9);

    // Cross boundary
    cam.AdvanceTime(0.1);

    // Advance past fade duration (1.5s)
    cam.AdvanceTime(2.0);
    // fadeTime = 0.1 + 2.0 = 2.1 > 1.5 → fade complete
    // Camera should follow path 1 normally at pathTime = 2.0, t = 0.2
    CHECK(cam.GetEye().y() == doctest::Approx(50.0));
    CHECK(cam.GetEye().z() == doctest::Approx(20.0));
}

TEST_CASE("fade smoothstep produces correct midpoint") {
    auto cam = makePerspCamera();
    cam.SetFadeEnabled(true);

    // Path 0: eye at (10,0,0), Path 1: eye at (0,10,0)
    // Both paths are static (same start and end keyframe eye)
    cam.LoadPaths({
        makePath(Vector3d(10, 0, 0), Vector3d(10, 0, 0), 10.0),
        makePath(Vector3d(0, 10, 0), Vector3d(0, 10, 0), 10.0),
    });

    // Advance to near end, then trigger transition
    cam.AdvanceTime(9.9);
    cam.AdvanceTime(0.1); // transition fires, fadeTime = 0.1

    // Advance to reach half of fade duration: fadeTime = 0.1 + 0.65 = 0.75
    cam.AdvanceTime(0.65);
    // smoothstep(0.5) = 0.5
    // eye should be midpoint: (5, 5, 0)
    CHECK(cam.GetEye().x() == doctest::Approx(5.0));
    CHECK(cam.GetEye().y() == doctest::Approx(5.0));
}

TEST_CASE("fade with single path wraps to self") {
    auto cam = makePerspCamera();
    cam.SetFadeEnabled(true);

    // Single path: eye 0→10, fades from end (10) back to start (0)
    cam.LoadPaths({ makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 5.0) });

    // Advance to near end
    cam.AdvanceTime(4.9);
    CHECK(cam.GetEye().x() == doctest::Approx(9.8));

    // Cross boundary
    cam.AdvanceTime(0.2);
    // Fading from ~(9.8,0,0) toward path start interpolation
    // Camera should be between 0 and 9.8, not snapped to 0
    CHECK(cam.GetEye().x() > 1.0); // still significantly offset from start
}

TEST_CASE("LoadPaths resets active fade") {
    auto cam = makePerspCamera();
    cam.SetFadeEnabled(true);

    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 5.0),
        makePath(Vector3d(0, 20, 0), Vector3d(0, 20, 10), 5.0),
    });

    // Trigger a fade
    cam.AdvanceTime(5.1);
    // Fade is active, camera is blending

    // Reload paths — should cancel the fade
    cam.LoadPaths({ makePath(Vector3d(100, 0, 0), Vector3d(200, 0, 0), 10.0) });
    CHECK(cam.GetEye().x() == doctest::Approx(100.0));

    // Advance — should follow new path without any fade blending
    cam.AdvanceTime(5.0);
    CHECK(cam.GetEye().x() == doctest::Approx(150.0));
}

TEST_CASE("fade does not affect camera when no path transition") {
    auto cam = makePerspCamera();
    cam.SetFadeEnabled(true);

    cam.LoadPaths({ makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 10.0) });

    // Advance within the same path — no fade should trigger
    cam.AdvanceTime(3.0);
    CHECK(cam.GetEye().x() == doctest::Approx(3.0));

    cam.AdvanceTime(2.0);
    CHECK(cam.GetEye().x() == doctest::Approx(5.0));
}

TEST_CASE("consecutive fades: new transition replaces active fade") {
    auto cam = makePerspCamera();
    cam.SetFadeEnabled(true);

    // 3 short paths
    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 2.0),
        makePath(Vector3d(0, 10, 0), Vector3d(0, 10, 10), 2.0),
        makePath(Vector3d(0, 0, 20), Vector3d(10, 0, 20), 2.0),
    });

    // Trigger first fade (path 0 → 1)
    cam.AdvanceTime(2.1);
    Vector3d afterFirstTransition = cam.GetEye();

    // Trigger second fade (path 1 → 2) while first fade is still active
    cam.AdvanceTime(2.0);
    // New fade should start from wherever camera was, not crash
    Vector3d afterSecondTransition = cam.GetEye();

    // Camera should be somewhere reasonable (blending toward path 2)
    cam.AdvanceTime(2.0);
    // After fade completes, should be on path 2
    CHECK(cam.GetEye().z() == doctest::Approx(20.0).epsilon(1.0));
}

} // TEST_SUITE
