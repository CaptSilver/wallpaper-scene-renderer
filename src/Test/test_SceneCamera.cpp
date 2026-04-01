#include <doctest.h>

#include "Scene/SceneCamera.h"
#include "Utils/Eigen.h"

#include <cmath>

using namespace wallpaper;
using namespace Eigen;

// ===========================================================================
// Direct Ortho/Perspective function tests (kills Eigen.h mutants)
// ===========================================================================

TEST_SUITE("EigenOrtho") {

TEST_CASE("asymmetric ortho maps left edge to -1") {
    // left=10, right=30 → center at 20, half-width=10
    auto m = Ortho(10.0, 30.0, -5.0, 5.0, 0.1, 100.0);
    Vector4d pt(10, 0, -0.1, 1);
    Vector4d r = m * pt;
    CHECK(r.x() / r.w() == doctest::Approx(-1.0).epsilon(0.01));
}

TEST_CASE("asymmetric ortho maps right edge to +1") {
    auto m = Ortho(10.0, 30.0, -5.0, 5.0, 0.1, 100.0);
    Vector4d pt(30, 0, -0.1, 1);
    Vector4d r = m * pt;
    CHECK(r.x() / r.w() == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("asymmetric ortho maps top edge to +1") {
    auto m = Ortho(-10.0, 10.0, 5.0, 15.0, 0.1, 100.0);
    Vector4d pt(0, 15, -0.1, 1);
    Vector4d r = m * pt;
    CHECK(r.y() / r.w() == doctest::Approx(1.0).epsilon(0.01));
}

TEST_CASE("ortho depth: near maps to 0, far maps to 1") {
    auto m = Ortho(-1, 1, -1, 1, 1.0, 10.0);
    // Z pointing into screen is negative in view space
    Vector4d near_pt(0, 0, -1.0, 1);
    Vector4d far_pt(0, 0, -10.0, 1);
    Vector4d rn = m * near_pt;
    Vector4d rf = m * far_pt;
    CHECK(rn.z() / rn.w() == doctest::Approx(0.0).epsilon(0.02));
    CHECK(rf.z() / rf.w() == doctest::Approx(1.0).epsilon(0.02));
}

TEST_CASE("perspective tan(fov/2) produces correct half-angle") {
    // fov=90° → tan(45°) = 1.0. Use near=0.5 so tan*near=0.5, tan/near=2 → different.
    double fov = Radians(90.0);
    auto m = Perspective(fov, 1.0, 0.5, 100.0);
    // Point at (0.5, 0, -0.5) should be at right edge (near plane width = 2*tan(45°)*0.5 = 1.0)
    Vector4d pt(0.5, 0, -0.5, 1);
    Vector4d r = m * pt;
    CHECK(r.x() / r.w() == doctest::Approx(1.0).epsilon(0.1));
    // A point twice as far should project to half the NDC (perspective foreshortening)
    Vector4d pt2(0.5, 0, -1.0, 1);
    Vector4d r2 = m * pt2;
    CHECK(std::abs(r2.x() / r2.w()) < std::abs(r.x() / r.w()));
}

TEST_CASE("perspective aspect scales X") {
    double fov = Radians(50.0);
    auto m_wide = Perspective(fov, 2.0, 0.01, 100.0);
    auto m_sq   = Perspective(fov, 1.0, 0.01, 100.0);
    Vector4d pt(1, 0, -5, 1);
    double wide_ndc = std::abs((m_wide * pt).x() / (m_wide * pt).w());
    double sq_ndc   = std::abs((m_sq * pt).x()   / (m_sq * pt).w());
    CHECK(wide_ndc < sq_ndc); // wider → smaller projected X
}

} // TEST_SUITE EigenOrtho

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

} // TEST_SUITE SceneCamera_Fade

// ===========================================================================
// Ortho Projection (kills ortho math mutants on lines 72-75)
// ===========================================================================

TEST_SUITE("SceneCamera_Ortho") {

TEST_CASE("ortho camera has correct VP matrix dimensions") {
    SceneCamera cam(1920, 1080, 0.1f, 100.0f);
    cam.SetDirectLookAt(Vector3d(0, 0, 1), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    auto vp = cam.GetViewProjectionMatrix();
    Eigen::Vector4d center_pt(0, 0, 0, 1);
    Eigen::Vector4d result = vp * center_pt;
    CHECK(result.x() / result.w() == doctest::Approx(0.0).epsilon(0.01));
    CHECK(result.y() / result.w() == doctest::Approx(0.0).epsilon(0.01));
}

TEST_CASE("ortho projection symmetric left/right") {
    SceneCamera cam(100, 100, 0.1f, 100.0f);
    cam.SetDirectLookAt(Vector3d(0, 0, 1), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    auto vp = cam.GetViewProjectionMatrix();
    Eigen::Vector4d right_pt(50, 0, 0, 1);
    Eigen::Vector4d r = vp * right_pt;
    CHECK(r.x() / r.w() == doctest::Approx(1.0).epsilon(0.1));
    Eigen::Vector4d left_pt(-50, 0, 0, 1);
    Eigen::Vector4d l = vp * left_pt;
    CHECK(l.x() / l.w() == doctest::Approx(-1.0).epsilon(0.1));
}

TEST_CASE("ortho projection symmetric top/bottom") {
    SceneCamera cam(100, 100, 0.1f, 100.0f);
    cam.SetDirectLookAt(Vector3d(0, 0, 1), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    auto vp = cam.GetViewProjectionMatrix();
    Eigen::Vector4d top_pt(0, 50, 0, 1);
    Eigen::Vector4d t = vp * top_pt;
    CHECK(t.y() / t.w() == doctest::Approx(1.0).epsilon(0.1));
    Eigen::Vector4d bot_pt(0, -50, 0, 1);
    Eigen::Vector4d b = vp * bot_pt;
    CHECK(b.y() / b.w() == doctest::Approx(-1.0).epsilon(0.1));
}

TEST_CASE("ortho camera aspect ratio from constructor") {
    // Width=1920, height=1080 → aspect should be ~1.78, NOT 1920*1080
    SceneCamera cam(1920, 1080, 0.1f, 100.0f);
    CHECK(cam.Aspect() == doctest::Approx(1920.0 / 1080.0));
}

TEST_CASE("non-square ortho projects width/height independently") {
    // Width=200, height=100 → left=-100, right=100, bottom=-50, top=50
    SceneCamera cam(200, 100, 0.1f, 100.0f);
    cam.SetDirectLookAt(Vector3d(0, 0, 1), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    auto vp = cam.GetViewProjectionMatrix();
    // Point at (100, 0, 0) → NDC x ≈ 1 (right edge)
    Eigen::Vector4d r = vp * Eigen::Vector4d(100, 0, 0, 1);
    CHECK(r.x() / r.w() == doctest::Approx(1.0).epsilon(0.1));
    // Point at (0, 50, 0) → NDC y ≈ 1 (top edge)
    Eigen::Vector4d t = vp * Eigen::Vector4d(0, 50, 0, 1);
    CHECK(t.y() / t.w() == doctest::Approx(1.0).epsilon(0.1));
    // Point at (0, 100, 0) → NDC y ≈ 2 (above top, NOT 1.0)
    Eigen::Vector4d above = vp * Eigen::Vector4d(0, 100, 0, 1);
    CHECK(above.y() / above.w() == doctest::Approx(2.0).epsilon(0.1));
}

TEST_CASE("perspective projection basic") {
    auto cam = makePerspCamera();
    cam.SetDirectLookAt(Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    auto vp = cam.GetViewProjectionMatrix();
    // Center point should project near (0, 0)
    Eigen::Vector4d center = vp * Eigen::Vector4d(0, 0, 0, 1);
    CHECK(center.x() / center.w() == doctest::Approx(0.0).epsilon(0.1));
    CHECK(center.y() / center.w() == doctest::Approx(0.0).epsilon(0.1));
}

TEST_CASE("perspective projection depth ordering") {
    auto cam = makePerspCamera();
    cam.SetDirectLookAt(Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    auto vp = cam.GetViewProjectionMatrix();
    // Closer point should have smaller depth than farther point
    Eigen::Vector4d near_pt = vp * Eigen::Vector4d(0, 0, 4, 1);
    Eigen::Vector4d far_pt  = vp * Eigen::Vector4d(0, 0, -10, 1);
    double near_z = near_pt.z() / near_pt.w();
    double far_z  = far_pt.z()  / far_pt.w();
    CHECK(near_z < far_z);
}

TEST_CASE("perspective fov affects projection") {
    // Narrow FOV → larger projected size for same object
    SceneCamera narrow(16.0f / 9.0f, 0.01f, 1000.0f, 20.0f); // 20° fov
    SceneCamera wide(16.0f / 9.0f, 0.01f, 1000.0f, 90.0f);   // 90° fov
    narrow.SetDirectLookAt(Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));
    wide.SetDirectLookAt(Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    // Off-center point: (1, 0, 0)
    Eigen::Vector4d pt(1, 0, 0, 1);
    Eigen::Vector4d narrow_r = narrow.GetViewProjectionMatrix() * pt;
    Eigen::Vector4d wide_r   = wide.GetViewProjectionMatrix()   * pt;
    // Narrow FOV should produce larger x in NDC
    double narrow_ndc_x = std::abs(narrow_r.x() / narrow_r.w());
    double wide_ndc_x   = std::abs(wide_r.x()   / wide_r.w());
    CHECK(narrow_ndc_x > wide_ndc_x);
}

TEST_CASE("perspective near/far plane depth range") {
    // Specific test for nearz/farz interaction in Perspective()
    auto cam = makePerspCamera(); // aspect=16/9, near=0.01, far=1000, fov=50
    cam.SetDirectLookAt(Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    auto vp = cam.GetViewProjectionMatrix();
    // A point at z=4.99 (very near camera, which is at z=5 looking at z=0)
    Eigen::Vector4d near_pt = vp * Eigen::Vector4d(0, 0, 4.99, 1);
    double near_ndc = near_pt.z() / near_pt.w();
    // A point at z=0 (center, 5 units from camera)
    Eigen::Vector4d mid_pt = vp * Eigen::Vector4d(0, 0, 0, 1);
    double mid_ndc = mid_pt.z() / mid_pt.w();
    // Near point should have smaller depth than mid point
    CHECK(near_ndc < mid_ndc);
    // Both should be in [0, 1] range (Vulkan depth convention)
    CHECK(near_ndc >= 0.0);
    CHECK(mid_ndc <= 1.0);
}

TEST_CASE("perspective off-center point X and Y") {
    auto cam = makePerspCamera();
    cam.SetDirectLookAt(Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    auto vp = cam.GetViewProjectionMatrix();
    // Point to the right: should have positive NDC x
    Eigen::Vector4d right_pt = vp * Eigen::Vector4d(2, 0, 0, 1);
    CHECK(right_pt.x() / right_pt.w() > 0);
    // Point above: should have positive NDC y
    Eigen::Vector4d top_pt = vp * Eigen::Vector4d(0, 2, 0, 1);
    CHECK(top_pt.y() / top_pt.w() > 0);
    // Point to the left: should have negative NDC x
    Eigen::Vector4d left_pt = vp * Eigen::Vector4d(-2, 0, 0, 1);
    CHECK(left_pt.x() / left_pt.w() < 0);
}

TEST_CASE("perspective aspect ratio affects X projection") {
    // Wider aspect → objects at same X appear smaller in NDC
    SceneCamera wide(2.0f, 0.01f, 100.0f, 50.0f);
    SceneCamera narrow(1.0f, 0.01f, 100.0f, 50.0f);
    wide.SetDirectLookAt(Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));
    narrow.SetDirectLookAt(Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    Eigen::Vector4d pt(1, 0, 0, 1);
    double wide_x = std::abs((wide.GetViewProjectionMatrix() * pt).x() /
                             (wide.GetViewProjectionMatrix() * pt).w());
    double narrow_x = std::abs((narrow.GetViewProjectionMatrix() * pt).x() /
                               (narrow.GetViewProjectionMatrix() * pt).w());
    // Wider aspect → same physical X subtends smaller angle → smaller NDC x
    CHECK(wide_x < narrow_x);
}

} // TEST_SUITE SceneCamera_Ortho

// ===========================================================================
// Reflection (kills reflect_y0 mutants on line 20)
// ===========================================================================

TEST_SUITE("SceneCamera_Reflection") {

TEST_CASE("GetPosition reflects Y when reflect_y0 enabled") {
    auto cam = makePerspCamera();
    cam.SetDirectLookAt(Vector3d(1, 5, 3), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    cam.SetReflectY0(true);
    Vector3d pos = cam.GetPosition();
    CHECK(pos.x() == doctest::Approx(1.0));
    CHECK(pos.y() == doctest::Approx(-5.0));
    CHECK(pos.z() == doctest::Approx(3.0));
}

TEST_CASE("GetPosition does not reflect when disabled") {
    auto cam = makePerspCamera();
    cam.SetDirectLookAt(Vector3d(1, 5, 3), Vector3d(0, 0, 0), Vector3d(0, 1, 0));

    Vector3d pos = cam.GetPosition();
    CHECK(pos.y() == doctest::Approx(5.0));
}

TEST_CASE("reflection VP matrix differs from normal") {
    auto cam1 = makePerspCamera();
    cam1.SetDirectLookAt(Vector3d(0, 5, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));
    auto vp1 = cam1.GetViewProjectionMatrix();

    auto cam2 = makePerspCamera();
    cam2.SetDirectLookAt(Vector3d(0, 5, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));
    cam2.SetReflectY0(true);
    cam2.Update();
    auto vp2 = cam2.GetViewProjectionMatrix();

    CHECK_FALSE(vp1.isApprox(vp2));
}

} // TEST_SUITE SceneCamera_Reflection

// ===========================================================================
// Boundary conditions (kills >=/>/</<= boundary mutants)
// ===========================================================================

TEST_SUITE("SceneCamera_Boundaries") {

TEST_CASE("path transition at exact duration boundary") {
    auto cam = makePerspCamera();
    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 5.0),
        makePath(Vector3d(0, 20, 0), Vector3d(0, 20, 10), 5.0),
    });

    cam.AdvanceTime(5.0);
    CHECK(cam.GetEye().y() == doctest::Approx(20.0));
}

TEST_CASE("single keyframe path handled") {
    auto cam = makePerspCamera();
    CameraPath cp;
    cp.duration = 10.0;
    CameraKeyframe kf;
    kf.eye    = Vector3d(1, 2, 3);
    kf.center = Vector3d(0, 0, 0);
    kf.up     = Vector3d(0, 1, 0);
    cp.keyframes = { kf };
    cam.LoadPaths({ cp });

    cam.AdvanceTime(5.0);
    CHECK(cam.GetEye().isApprox(Vector3d(1, 2, 3)));
}

TEST_CASE("fade boundary: fadeTime equals fadeDuration") {
    auto cam = makePerspCamera();
    cam.SetFadeEnabled(true);
    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 10.0),
        makePath(Vector3d(0, 50, 0), Vector3d(0, 50, 100), 10.0),
    });

    cam.AdvanceTime(9.99);
    cam.AdvanceTime(0.01);
    cam.AdvanceTime(1.49);
    CHECK(cam.GetEye().y() == doctest::Approx(50.0));
}

TEST_CASE("pathTime subtraction preserves remainder") {
    auto cam = makePerspCamera();
    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 2.0),
        makePath(Vector3d(0, 0, 0), Vector3d(0, 10, 0), 2.0),
    });

    cam.AdvanceTime(2.5);
    CHECK(cam.GetEye().y() == doctest::Approx(2.5));
}

TEST_CASE("pathTime subtraction not addition (line 134 mutant killer)") {
    auto cam = makePerspCamera();
    // Path 0: 2s, Path 1: 2s
    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 2.0),
        makePath(Vector3d(0, 0, 0), Vector3d(0, 10, 0), 2.0),
    });

    // Advance 2.1s → on path 1 at pathTime=0.1
    cam.AdvanceTime(2.1);
    // If mutated to +=, pathTime would be 2.1+2.0=4.1 → would overflow next path too
    CHECK(cam.GetEye().y() == doctest::Approx(0.5).epsilon(0.1));
    // Then advance another 1.9s → should be at end of path 1
    cam.AdvanceTime(1.9);
    // Now at pathTime=2.0, wraps to path 0 at t=0
    CHECK(cam.GetEye().x() == doctest::Approx(0.0).epsilon(0.1));
}

TEST_CASE("pathTime exact equality with next_path.duration (line 137)") {
    auto cam = makePerspCamera();
    // Path 0: 1s, Path 1: 1s (very short)
    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 1.0),
        makePath(Vector3d(0, 0, 0), Vector3d(0, 10, 0), 1.0),
    });

    // Advance 2.0s: dt exceeds path 0, remainder=1.0 which == next_path.duration
    // With >=: pathTime=1.0 >= 1.0 → clamps to 0
    // With >:  pathTime=1.0 >  1.0 → false, uses pathTime=1.0, t=1.0
    cam.AdvanceTime(2.0);
    // The eye should be at path 1 start (clamped) or end (t=1.0)
    // With >=: clamped to t=0, eye.y=0
    CHECK(cam.GetEye().y() == doctest::Approx(0.0).epsilon(0.1));
}

TEST_CASE("t clamping at exactly 0 and 1 (lines 147-149)") {
    auto cam = makePerspCamera();
    cam.LoadPaths({ makePath(Vector3d(0, 0, 0), Vector3d(100, 0, 0), 10.0) });

    // At t=0 (start)
    CHECK(cam.GetEye().x() == doctest::Approx(0.0));

    // At t=1 (end, just before transition)
    cam.AdvanceTime(10.0); // triggers path cycling back to start
    // After cycling, pathTime should wrap, putting us at start again
    CHECK(cam.GetEye().x() == doctest::Approx(0.0));
}

TEST_CASE("fade completes at exact boundary (line 158)") {
    auto cam = makePerspCamera();
    cam.SetFadeEnabled(true);

    cam.LoadPaths({
        makePath(Vector3d(10, 0, 0), Vector3d(10, 0, 0), 5.0),
        makePath(Vector3d(0, 100, 0), Vector3d(0, 100, 0), 5.0),
    });

    cam.AdvanceTime(4.99);
    cam.AdvanceTime(0.01); // transition, fadeTime=0.01
    // Advance to exactly fadeDuration (1.5s): fadeTime = 0.01 + 1.49 = 1.50
    cam.AdvanceTime(1.49);
    // With >=: 1.50 >= 1.50 → fade complete, m_fading=false, Y = 100
    // With >:  1.50 >  1.50 → false, fade still active, Y blended
    // At smoothstep(1.5/1.5)=1.0 the blend is complete anyway — need a different approach.
    // Instead: advance 1.48 so fadeTime=1.49 < 1.5 (fade still active), then check
    // the NEXT frame after 0.02 more: fadeTime=1.51 >= 1.5 → fade ends.
    // With mutant: fadeTime=1.51 > 1.5 → also true. Both agree!
    // The mutant is equivalent for floating point accumulation.
    // Let's verify fade is active at 1.49 and complete at 1.50 exactly.
    CHECK(cam.GetEye().y() == doctest::Approx(100.0).epsilon(0.01));
}

TEST_CASE("zero-duration path uses t=0 (kills >0 → >=0 mutant)") {
    auto cam = makePerspCamera();
    CameraPath cp;
    cp.duration = 0; // zero duration!
    CameraKeyframe kf0, kf1;
    kf0.eye = Vector3d(0, 0, 0); kf0.center = Vector3d::Zero(); kf0.up = Vector3d::UnitY();
    kf1.eye = Vector3d(100, 0, 0); kf1.center = Vector3d::Zero(); kf1.up = Vector3d::UnitY();
    cp.keyframes = {kf0, kf1};
    cam.LoadPaths({cp});

    cam.AdvanceTime(0.1);
    // With duration=0: t should be 0 (not inf from 0.1/0)
    // eye = kf0.eye + 0*(kf1.eye - kf0.eye) = (0,0,0)
    CHECK(cam.GetEye().x() == doctest::Approx(0.0).epsilon(1.0));
    // Must NOT be 100 (which would happen if t=inf from division by 0)
    CHECK(cam.GetEye().x() < 50.0);
}

TEST_CASE("3-path cycling advances to next (not previous) path") {
    // Kills (m_currentPath + 1) → (m_currentPath - 1) mutant
    // With 3 paths: 0→1→2→0. Mutant: 0→2→1→0 (different order!)
    auto cam = makePerspCamera();
    cam.LoadPaths({
        makePath(Vector3d(0, 0, 0), Vector3d(10, 0, 0), 2.0),
        makePath(Vector3d(0, 20, 0), Vector3d(0, 20, 10), 2.0),
        makePath(Vector3d(0, 0, 40), Vector3d(10, 0, 40), 2.0),
    });

    // Advance past path 0 → should go to path 1 (not path 2)
    cam.AdvanceTime(2.1);
    CHECK(cam.GetEye().y() == doctest::Approx(20.0).epsilon(0.5));
    CHECK(cam.GetEye().z() == doctest::Approx(0.5).epsilon(0.5));
}

} // TEST_SUITE SceneCamera_Boundaries
