#include <doctest.h>

#include "WPPuppet.hpp"

#include <cmath>
#include <memory>

using namespace wallpaper;

namespace
{
// Helper: create a simple Animation with known parameters
WPPuppet::Animation makeAnimation(WPPuppet::PlayMode mode, int length, double fps) {
    WPPuppet::Animation anim;
    anim.id     = 0;
    anim.fps    = fps;
    anim.length = length;
    anim.mode   = mode;
    anim.name   = "test";
    // prepared() equivalent
    anim.frame_time = 1.0 / fps;
    anim.max_time   = (double)length / fps;
    return anim;
}
} // namespace

// ===========================================================================
// Animation::getInterpolationInfo — Loop mode
// ===========================================================================

TEST_SUITE("WPPuppet_Loop") {

TEST_CASE("Loop mode frame 0 at t=0") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Loop, 10, 30.0);
    double t  = 0.0;
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 0);
    CHECK(info.frame_b == 1);
    CHECK(info.t == doctest::Approx(0.0));
}

TEST_CASE("Loop mode mid frame interpolation") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Loop, 10, 10.0);
    // frame_time = 0.1, so t=0.15 should be midway between frame 1 and 2
    double t  = 0.15;
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 1);
    CHECK(info.frame_b == 2);
    CHECK(info.t == doctest::Approx(0.5));
}

TEST_CASE("Loop mode wraps at end") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Loop, 10, 10.0);
    // max_time = 1.0, so t=1.05 should wrap to 0.05 = frame 0 with t=0.5
    double t  = 1.05;
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 0);
    CHECK(info.frame_b == 1);
    CHECK(info.t == doctest::Approx(0.5));
}

TEST_CASE("Loop mode last frame wraps to first") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Loop, 10, 10.0);
    // t = 0.95 should be frame 9, next = frame 0 (wraps)
    double t  = 0.95;
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 9);
    CHECK(info.frame_b == 0);
    CHECK(info.t == doctest::Approx(0.5));
}

} // TEST_SUITE

// ===========================================================================
// Animation::getInterpolationInfo — Single mode
// ===========================================================================

TEST_SUITE("WPPuppet_Single") {

TEST_CASE("Single mode clamps at end") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Single, 10, 10.0);
    double t  = 2.0; // way past max_time (1.0)
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 9);
    CHECK(info.frame_b == 9);
    CHECK(info.t == doctest::Approx(0.0));
    CHECK(t == doctest::Approx(1.0)); // cur_time clamped to max_time
}

TEST_CASE("Single mode before end plays normally") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Single, 10, 10.0);
    double t  = 0.35;
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 3);
    CHECK(info.frame_b == 4);
    CHECK(info.t == doctest::Approx(0.5));
}

TEST_CASE("Single mode at exactly max_time") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Single, 10, 10.0);
    double t  = 1.0;
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 9);
    CHECK(info.frame_b == 9);
    CHECK(info.t == doctest::Approx(0.0));
}

} // TEST_SUITE

// ===========================================================================
// Animation::getInterpolationInfo — Mirror mode
// ===========================================================================

TEST_SUITE("WPPuppet_Mirror") {

TEST_CASE("Mirror mode forward half") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Mirror, 10, 10.0);
    // Forward pass: t=0.25 should be frame 2→3 with t=0.5
    double t  = 0.25;
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 2);
    CHECK(info.frame_b == 3);
    CHECK(info.t == doctest::Approx(0.5));
}

TEST_CASE("Mirror mode backward half") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Mirror, 10, 10.0);
    // Backward pass: t=1.5 means 15 frames into doubled length (20 frames)
    // frame 15 in doubled: _get_frame(15) = (10-1) - (15-10) = 9-5 = 4
    // frame 16: _get_frame(16) = (10-1) - (16-10) = 9-6 = 3
    double t  = 1.55;
    auto info = anim.getInterpolationInfo(&t);
    // 15.5 frames into doubled: frame_a=15, frame_b=16, t=0.5
    // _get_frame(15) = 4, _get_frame(16) = 3
    CHECK(info.frame_a == 4);
    CHECK(info.frame_b == 3);
    CHECK(info.t == doctest::Approx(0.5));
}

TEST_CASE("Mirror mode at start is same as loop") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Mirror, 10, 10.0);
    double t  = 0.05;
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 0);
    CHECK(info.frame_b == 1);
    CHECK(info.t == doctest::Approx(0.5));
}

} // TEST_SUITE
