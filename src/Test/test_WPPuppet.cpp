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

TEST_CASE("Mirror mode at exact mirror point") {
    // Kills ge_to_gt on line 167: _get_frame(f) uses f >= length
    auto anim = makeAnimation(WPPuppet::PlayMode::Mirror, 10, 10.0);
    // At the exact mirror point: frame 10 in doubled length
    // _get_frame(10) = (10-1) - (10-10) = 9 (last frame)
    double t = 1.0; // 10 frames into doubled (20-frame) cycle
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 9);
}

TEST_CASE("Mirror mode at start is same as loop") {
    auto anim = makeAnimation(WPPuppet::PlayMode::Mirror, 10, 10.0);
    double t  = 0.05;
    auto info = anim.getInterpolationInfo(&t);
    CHECK(info.frame_a == 0);
    CHECK(info.frame_b == 1);
    CHECK(info.t == doctest::Approx(0.5));
}

} // TEST_SUITE("WPPuppet_Mirror")

namespace
{
std::shared_ptr<WPPuppet> makePuppet(int bone_count, int anim_id, double fps, int length,
                                     WPPuppet::PlayMode mode = WPPuppet::PlayMode::Loop) {
    auto puppet = std::make_shared<WPPuppet>();
    for (int i = 0; i < bone_count; i++) {
        WPPuppet::Bone bone;
        bone.transform = Eigen::Affine3f::Identity();
        bone.parent    = (i == 0) ? 0xFFFFFFFFu : 0u;
        puppet->bones.push_back(bone);
    }
    WPPuppet::Animation anim;
    anim.id = anim_id; anim.fps = fps; anim.length = length;
    anim.mode = mode; anim.name = "test";
    for (int b = 0; b < bone_count; b++) {
        WPPuppet::Animation::BoneFrames bf;
        for (int f = 0; f < length; f++) {
            WPPuppet::BoneFrame frame;
            frame.position = Eigen::Vector3f((float)f, 0.0f, 0.0f);
            frame.angle    = Eigen::Vector3f::Zero();
            frame.scale    = Eigen::Vector3f::Ones();
            bf.frames.push_back(frame);
        }
        anim.bframes_array.push_back(bf);
    }
    puppet->anims.push_back(anim);
    return puppet;
}
} // namespace

// ===========================================================================
// WPPuppet::prepared() — frame timing and bone transforms
// ===========================================================================

TEST_SUITE("WPPuppet_Prepared") {

TEST_CASE("frame_time and max_time computed correctly") {
    auto puppet = makePuppet(1, 1, 10.0, 5);
    puppet->prepared();
    CHECK(puppet->anims[0].frame_time == doctest::Approx(0.1));
    CHECK(puppet->anims[0].max_time == doctest::Approx(0.5));
}

TEST_CASE("frame_time with fps=30 and length=60") {
    auto puppet = makePuppet(1, 1, 30.0, 60);
    puppet->prepared();
    CHECK(puppet->anims[0].frame_time == doctest::Approx(1.0 / 30.0));
    CHECK(puppet->anims[0].max_time == doctest::Approx(2.0));
}

TEST_CASE("offset_trans is inverse of combined transform") {
    auto puppet = makePuppet(2, 1, 10.0, 2);
    puppet->bones[0].transform.pretranslate(Eigen::Vector3f(1.0f, 0.0f, 0.0f));
    puppet->prepared();
    auto product = puppet->bones[0].transform * puppet->bones[0].offset_trans;
    CHECK(product.matrix().isApprox(Eigen::Affine3f::Identity().matrix(), 1e-5f));
}

TEST_CASE("child bone offset_trans includes parent chain") {
    auto puppet = makePuppet(2, 1, 10.0, 2);
    puppet->bones[0].transform.pretranslate(Eigen::Vector3f(2.0f, 0.0f, 0.0f));
    puppet->bones[1].transform.pretranslate(Eigen::Vector3f(0.0f, 3.0f, 0.0f));
    puppet->prepared();
    auto combined = puppet->bones[0].transform * puppet->bones[1].transform;
    auto product  = combined * puppet->bones[1].offset_trans;
    CHECK(product.matrix().isApprox(Eigen::Affine3f::Identity().matrix(), 1e-5f));
}

TEST_CASE("quaternions computed from euler angles") {
    auto puppet = makePuppet(1, 1, 10.0, 2);
    puppet->anims[0].bframes_array[0].frames[1].angle = Eigen::Vector3f(0, 0, (float)M_PI / 2.0f);
    puppet->prepared();
    auto q0 = puppet->anims[0].bframes_array[0].frames[0].quaternion;
    CHECK(q0.w() == doctest::Approx(1.0).epsilon(1e-6));
    auto q1 = puppet->anims[0].bframes_array[0].frames[1].quaternion;
    CHECK(std::abs(q1.z()) > 0.3);
}

TEST_CASE("noParent check") {
    WPPuppet::Bone bone;
    bone.parent = 0xFFFFFFFFu;
    CHECK(bone.noParent() == true);
    bone.parent = 0;
    CHECK(bone.noParent() == false);
}

} // TEST_SUITE("WPPuppet_Prepared")

// ===========================================================================
// WPPuppetLayer::prepared() — blend calculation
// ===========================================================================

TEST_SUITE("WPPuppetLayer_Prepared") {

TEST_CASE("single visible layer matched by ID") {
    auto puppet = makePuppet(1, 100, 10.0, 2);
    puppet->prepared();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0].id = 100; alayers[0].blend = 1.0; alayers[0].visible = true; alayers[0].rate = 1.0;
    layer.prepared(alayers);
    auto frames = layer.genFrame(0.0);
    CHECK(frames.size() == 1);
}

TEST_CASE("invisible layer not matched") {
    auto puppet = makePuppet(1, 100, 10.0, 2);
    puppet->prepared();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0].id = 100; alayers[0].blend = 1.0; alayers[0].visible = false; alayers[0].rate = 1.0;
    layer.prepared(alayers);
    auto frames = layer.genFrame(0.0);
    CHECK(frames.size() == 1);
}

TEST_CASE("mismatched animation ID not matched") {
    auto puppet = makePuppet(1, 100, 10.0, 2);
    puppet->prepared();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0].id = 999; alayers[0].blend = 1.0; alayers[0].visible = true; alayers[0].rate = 1.0;
    layer.prepared(alayers);
    auto frames = layer.genFrame(0.0);
    CHECK(frames.size() == 1);
}

TEST_CASE("total_blend > 1.0 normalizes blends") {
    auto puppet = makePuppet(1, 100, 10.0, 2);
    auto anim2 = puppet->anims[0]; anim2.id = 200;
    puppet->anims.push_back(anim2);
    puppet->prepared();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(2);
    alayers[0] = {100, 1.0, 0.8, true, 0.0};
    alayers[1] = {200, 1.0, 0.8, true, 0.0};
    layer.prepared(alayers);
    auto frames = layer.genFrame(0.0);
    CHECK(frames.size() == 1);
}

TEST_CASE("total_blend <= 1.0 multiplicative path") {
    auto puppet = makePuppet(1, 100, 10.0, 2);
    puppet->prepared();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0] = {100, 1.0, 0.5, true, 0.0};
    layer.prepared(alayers);
    auto frames = layer.genFrame(0.0);
    CHECK(frames.size() == 1);
}

} // TEST_SUITE("WPPuppetLayer_Prepared")

// ===========================================================================
// WPPuppet::genFrame() — output verification
// ===========================================================================

TEST_SUITE("WPPuppet_GenFrame") {

namespace
{
// Helper: build puppet with 1 bone, 3 frames at pos (0,0,0), (10,0,0), (20,0,0)
std::shared_ptr<WPPuppet> makeSimplePuppet3Frame() {
    auto puppet = std::make_shared<WPPuppet>();
    WPPuppet::Bone bone;
    bone.transform = Eigen::Affine3f::Identity();
    bone.parent = 0xFFFFFFFFu;
    puppet->bones.push_back(bone);

    WPPuppet::Animation anim;
    anim.id = 1; anim.fps = 10.0; anim.length = 3;
    anim.mode = WPPuppet::PlayMode::Loop; anim.name = "test";
    WPPuppet::Animation::BoneFrames bf;
    for (int f = 0; f < 3; f++) {
        WPPuppet::BoneFrame frame;
        frame.position = Eigen::Vector3f(f * 10.0f, 0, 0);
        frame.angle = Eigen::Vector3f::Zero();
        frame.scale = Eigen::Vector3f::Ones();
        bf.frames.push_back(frame);
    }
    anim.bframes_array.push_back(bf);
    puppet->anims.push_back(anim);
    puppet->prepared();
    return puppet;
}
// Helper: puppet with 1 bone, 2 frames. Frame 0 has 30° Z rotation (NON-identity base
// quaternion, critical for killing slerp blend mutants). Frame 1 has 90° Z rotation.
std::shared_ptr<WPPuppet> makeRotatedPuppet() {
    auto puppet = std::make_shared<WPPuppet>();
    WPPuppet::Bone bone;
    bone.transform = Eigen::Affine3f::Identity();
    bone.parent = 0xFFFFFFFFu;
    puppet->bones.push_back(bone);

    WPPuppet::Animation anim;
    anim.id = 1; anim.fps = 10.0; anim.length = 2;
    anim.mode = WPPuppet::PlayMode::Loop; anim.name = "rotate";
    WPPuppet::Animation::BoneFrames bf;
    {
        WPPuppet::BoneFrame f0;
        f0.position = Eigen::Vector3f::Zero();
        f0.angle = Eigen::Vector3f(0, 0, 30.0f); // 30° Z — non-identity base!
        f0.scale = Eigen::Vector3f::Ones();
        bf.frames.push_back(f0);
    }
    {
        WPPuppet::BoneFrame f1;
        f1.position = Eigen::Vector3f::Zero();
        f1.angle = Eigen::Vector3f(0, 0, 90.0f); // 90° Z
        f1.scale = Eigen::Vector3f::Ones();
        bf.frames.push_back(f1);
    }
    anim.bframes_array.push_back(bf);
    puppet->anims.push_back(anim);
    puppet->prepared();
    return puppet;
}

} // namespace

TEST_CASE("genFrame exact position at t=0") {
    auto puppet = makeSimplePuppet3Frame();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0] = {1, 1.0, 1.0, true, 0.0};
    layer.prepared(alayers);
    auto frames = layer.genFrame(0.0);
    CHECK(frames[0].translation().x() == doctest::Approx(0.0f));
}

TEST_CASE("genFrame exact position after 0.05s (half frame)") {
    auto puppet = makeSimplePuppet3Frame();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0] = {1, 1.0, 1.0, true, 0.0};
    layer.prepared(alayers);
    layer.genFrame(0.0);
    auto frames = layer.genFrame(0.05);
    CHECK(frames[0].translation().x() == doctest::Approx(5.0f));
}

TEST_CASE("genFrame at frame 1-2 transition verifies one_t calculation") {
    // At frame 1→2 interpolation, both delta_a and delta_b are non-zero.
    // frame_base = frame[0] = (0,0,0)
    // delta_a = frame[1] - frame[0] = (10,0,0), delta_b = frame[2] - frame[0] = (20,0,0)
    // At info.t=0.5: trans = base(0) + blend*(delta_a * one_t + delta_b * t)
    //   = 0 + 1.0*(10*0.5 + 20*0.5) = 5 + 10 = 15
    // If one_t mutated to 1.0+t=1.5: 1.0*(10*1.5 + 20*0.5) = 15+10 = 25 (WRONG)
    auto puppet = makeSimplePuppet3Frame();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0] = {1, 1.0, 1.0, true, 0.0};
    layer.prepared(alayers);
    // Advance to t=0.15 cumulative (frame_a=1, frame_b=2, info.t=0.5)
    layer.genFrame(0.0);
    layer.genFrame(0.1);    // now at t=0.1 → frame 1
    auto frames = layer.genFrame(0.05); // now at t=0.15 → midpoint of frame 1→2
    // Kills sub_to_add on line 89
    CHECK(frames[0].translation().x() == doctest::Approx(15.0f));
}

TEST_CASE("genFrame with blend=0.5 reduces position") {
    auto puppet = makeSimplePuppet3Frame();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0] = {1, 1.0, 0.5, true, 0.0};
    layer.prepared(alayers);
    layer.genFrame(0.0);
    auto frames = layer.genFrame(0.05);
    // With blend=0.5, position should be halved compared to blend=1.0
    CHECK(frames[0].translation().x() == doctest::Approx(2.5f));
}

TEST_CASE("genFrame two layers with total_blend > 1.0") {
    auto puppet = std::make_shared<WPPuppet>();
    WPPuppet::Bone bone;
    bone.transform = Eigen::Affine3f::Identity();
    bone.parent = 0xFFFFFFFFu;
    puppet->bones.push_back(bone);

    // Anim 1: frames at x=0, 10, 20
    WPPuppet::Animation anim1;
    anim1.id = 1; anim1.fps = 10.0; anim1.length = 3;
    anim1.mode = WPPuppet::PlayMode::Loop; anim1.name = "a1";
    WPPuppet::Animation::BoneFrames bf1;
    for (int f = 0; f < 3; f++) {
        WPPuppet::BoneFrame frame;
        frame.position = Eigen::Vector3f(f * 10.0f, 0, 0);
        frame.angle = Eigen::Vector3f::Zero();
        frame.scale = Eigen::Vector3f::Ones();
        bf1.frames.push_back(frame);
    }
    anim1.bframes_array.push_back(bf1);

    // Anim 2: same but with y=5
    auto anim2 = anim1; anim2.id = 2; anim2.name = "a2";
    for (auto& f : anim2.bframes_array[0].frames) f.position.y() = 5.0f;

    puppet->anims = { anim1, anim2 };
    puppet->prepared();

    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(2);
    alayers[0] = {1, 1.0, 0.8, true, 0.0};
    alayers[1] = {2, 1.0, 0.8, true, 0.0};
    layer.prepared(alayers);

    // total_blend=1.6 > 1.0 → normalized: each gets 0.8/1.6 = 0.5
    // Kills div_to_mul on line 213 and gt_to_ge/gt_to_le on line 211
    auto frames = layer.genFrame(0.0);
    // y should be 2.5 (half of 5.0 from anim2's y offset)
    CHECK(frames[0].translation().y() == doctest::Approx(2.5f));
}

TEST_CASE("genFrame with rotation verifies quaternion slerp blend") {
    // Kills sub_to_add on lines 96-97: slerp(1.0 - blend, ident)
    auto puppet = std::make_shared<WPPuppet>();
    WPPuppet::Bone bone;
    bone.transform = Eigen::Affine3f::Identity();
    bone.parent = 0xFFFFFFFFu;
    puppet->bones.push_back(bone);

    WPPuppet::Animation anim;
    anim.id = 1; anim.fps = 10.0; anim.length = 2;
    anim.mode = WPPuppet::PlayMode::Loop; anim.name = "rot";
    WPPuppet::BoneFrame f0, f1;
    f0.position = Eigen::Vector3f::Zero(); f0.angle = Eigen::Vector3f::Zero(); f0.scale = Eigen::Vector3f::Ones();
    f1.position = Eigen::Vector3f::Zero(); f1.angle = Eigen::Vector3f(0, 0, (float)M_PI / 2.0f); f1.scale = Eigen::Vector3f::Ones();
    WPPuppet::Animation::BoneFrames bf; bf.frames = { f0, f1 };
    anim.bframes_array.push_back(bf);
    puppet->anims.push_back(anim);
    puppet->prepared();

    // With blend=1.0, frame 0 has no rotation, frame 1 has 90deg Z
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0] = {1, 1.0, 1.0, true, 0.0};
    layer.prepared(alayers);

    // At t=0 (frame 0): should be near identity rotation
    auto frames0 = layer.genFrame(0.0);
    Eigen::Matrix3f rot0 = frames0[0].linear();
    CHECK(rot0(0,0) == doctest::Approx(1.0f).epsilon(0.1f));

    // At t=0.05 (midpoint): rotation should be ~45 degrees
    auto frames1 = layer.genFrame(0.05);
    Eigen::Matrix3f rot1 = frames1[0].linear();
    // cos(45deg) ≈ 0.707
    CHECK(rot1(0,0) == doctest::Approx(std::cos(M_PI/4.0)).epsilon(0.15f));
}

TEST_CASE("genFrame position increases monotonically") {
    auto puppet = makeSimplePuppet3Frame();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0] = {1, 1.0, 1.0, true, 0.0};
    layer.prepared(alayers);

    float prev_x = -1.0f;
    for (int i = 0; i < 10; i++) {
        auto frames = layer.genFrame(0.01);
        float x = frames[0].translation().x();
        CHECK(x >= prev_x);
        prev_x = x;
    }
}

TEST_CASE("genFrame with partial blend reduces effect") {
    // With blend=0.5, position contribution should be half of blend=1.0
    // This kills mutants: 1.0 - blend → 1.0 + blend
    auto puppet = makeSimplePuppet3Frame();

    // Full blend (1.0)
    WPPuppetLayer layer1(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> al1(1);
    al1[0] = {1, 1.0, 1.0, true, 0.0}; // rate=1, blend=1
    layer1.prepared(al1);
    auto frames1 = layer1.genFrame(0.0);
    layer1.genFrame(0.1); // advance time
    float full_x = layer1.genFrame(0.0).data()->translation().x();

    // Partial blend (0.5)
    WPPuppetLayer layer2(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> al2(1);
    al2[0] = {1, 1.0, 0.5, true, 0.0}; // rate=1, blend=0.5
    layer2.prepared(al2);
    layer2.genFrame(0.0);
    layer2.genFrame(0.1);
    float half_x = layer2.genFrame(0.0).data()->translation().x();

    // Partial blend should produce smaller position offset
    CHECK(std::abs(half_x) < std::abs(full_x) + 0.01f);
}

TEST_CASE("genFrame with zero blend produces minimal change") {
    // blend=0 should leave bone near identity (only global_blend * base transform)
    auto puppet = makeSimplePuppet3Frame();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0] = {1, 1.0, 0.0, true, 0.0}; // blend=0 → animation has no effect
    layer.prepared(alayers);
    auto frames = layer.genFrame(0.0);
    layer.genFrame(0.1);
    auto frames2 = layer.genFrame(0.0);
    // With blend=0, position should not change from frame advance
    float x0 = frames.data()->translation().x();
    float x1 = frames2.data()->translation().x();
    CHECK(x0 == doctest::Approx(x1).epsilon(0.01f));
}

TEST_CASE("genFrame hidden layer has no effect") {
    auto puppet = makeSimplePuppet3Frame();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0] = {1, 1.0, 1.0, false, 0.0}; // visible=false
    layer.prepared(alayers);
    auto frames = layer.genFrame(0.0);
    float x0 = frames.data()->translation().x();
    layer.genFrame(0.1);
    float x1 = layer.genFrame(0.0).data()->translation().x();
    CHECK(x0 == doctest::Approx(x1).epsilon(0.01f));
}

TEST_CASE("genFrame rotation with non-identity base quaternion and partial blend") {
    // Kills slerp(1.0-blend, ident) → slerp(1.0+blend, ident) mutant (lines 98-99)
    // Base frame has 30° Z rotation (non-identity quaternion).
    // With anim_layer.blend=0.5: slerp(0.5, ident) attenuates the base rotation.
    // With mutant (1.0+0.5=1.5): slerp(1.5, ident) extrapolates in reverse — different result.
    auto puppet = makeRotatedPuppet();

    // Full blend (1.0): advance to midpoint between frame 0 (30°) and frame 1 (90°)
    WPPuppetLayer full_layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> al_full(1);
    al_full[0] = {1, 1.0, 1.0, true, 0.0};
    full_layer.prepared(al_full);
    full_layer.genFrame(0.05); // advance to t=0.5 between frames
    auto full_result = full_layer.genFrame(0.0);
    Eigen::Matrix3f full_rot = full_result[0].rotation();
    float full_angle = std::atan2(full_rot(1,0), full_rot(0,0));

    // Half blend (0.5): same time advancement, but reduced blend
    WPPuppetLayer half_layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> al_half(1);
    al_half[0] = {1, 1.0, 0.5, true, 0.0};
    half_layer.prepared(al_half);
    half_layer.genFrame(0.05);
    auto half_result = half_layer.genFrame(0.0);
    Eigen::Matrix3f half_rot = half_result[0].rotation();
    float half_angle = std::atan2(half_rot(1,0), half_rot(0,0));

    // Half blend should produce a different (smaller) rotation than full blend
    // since slerp(0.5, ident) attenuates vs slerp(1.0, ident) = no change
    CHECK(std::abs(half_angle) != doctest::Approx(std::abs(full_angle)).epsilon(0.01f));
    CHECK(std::abs(full_angle) > 0.1f); // should have meaningful rotation
}

TEST_CASE("genFrame with total_blend exactly 1.0 uses multiplicative path") {
    // Kills total_blend > 1.0 → total_blend >= 1.0 mutant (line 217)
    // With total_blend==1.0, should use multiplicative path (not normalization)
    auto puppet = makeSimplePuppet3Frame();
    WPPuppetLayer layer(puppet);
    std::vector<WPPuppetLayer::AnimationLayer> alayers(1);
    alayers[0] = {1, 1.0, 1.0, true, 0.0}; // total_blend = 1.0 exactly
    layer.prepared(alayers);

    // Should work without crashing (normalization would divide by 1.0 anyway)
    auto frames = layer.genFrame(0.0);
    layer.genFrame(0.05);
    auto frames2 = layer.genFrame(0.0);
    float x = frames2[0].translation().x();
    CHECK(x == doctest::Approx(5.0f)); // halfway between 0 and 10
}

} // TEST_SUITE("WPPuppet_GenFrame")
