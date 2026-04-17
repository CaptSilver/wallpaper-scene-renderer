#include <doctest.h>
#include "WPVolumeAnimation.h"

using wallpaper::EvaluateVolumeAnimation;
using wallpaper::VolumeAnimKeyframe;

// Helper: build the trainsound "vrom" animation from wallpaper 2477602742
static std::vector<VolumeAnimKeyframe> makeVromKeyframes() {
    return {
        { 0, 0.0f },
        { 26, 0.0f },
        { 28, 0.6f },
        { 30, 0.6f },
        { 32, 0.0f }
    };
}

TEST_SUITE("EvaluateVolumeAnimation") {

TEST_CASE("empty keyframes returns 0") {
    CHECK(EvaluateVolumeAnimation({}, 1, 100, "loop", 0) == 0.0f);
}

TEST_CASE("single keyframe returns its value") {
    CHECK(EvaluateVolumeAnimation({{ 0, 0.5f }}, 1, 100, "loop", 50) == 0.5f);
}

TEST_CASE("vrom: silent at t=0") {
    auto kfs = makeVromKeyframes();
    CHECK(EvaluateVolumeAnimation(kfs, 1, 360, "loop", 0) == 0.0f);
}

TEST_CASE("vrom: silent at t=25 (before ramp)") {
    auto kfs = makeVromKeyframes();
    CHECK(EvaluateVolumeAnimation(kfs, 1, 360, "loop", 25) == 0.0f);
}

TEST_CASE("vrom: midway ramp at t=27") {
    auto kfs = makeVromKeyframes();
    float v = EvaluateVolumeAnimation(kfs, 1, 360, "loop", 27);
    CHECK(v == doctest::Approx(0.3f)); // linear interp: 50% of 0→0.6
}

TEST_CASE("vrom: peak at t=28") {
    auto kfs = makeVromKeyframes();
    CHECK(EvaluateVolumeAnimation(kfs, 1, 360, "loop", 28) == doctest::Approx(0.6f));
}

TEST_CASE("vrom: holds at t=29") {
    auto kfs = makeVromKeyframes();
    float v = EvaluateVolumeAnimation(kfs, 1, 360, "loop", 29);
    CHECK(v == doctest::Approx(0.6f));
}

TEST_CASE("vrom: ramp down at t=31") {
    auto kfs = makeVromKeyframes();
    float v = EvaluateVolumeAnimation(kfs, 1, 360, "loop", 31);
    CHECK(v == doctest::Approx(0.3f)); // 50% of 0.6→0
}

TEST_CASE("vrom: silent at t=32") {
    auto kfs = makeVromKeyframes();
    CHECK(EvaluateVolumeAnimation(kfs, 1, 360, "loop", 32) == 0.0f);
}

TEST_CASE("vrom: silent at t=100 (after last keyframe)") {
    auto kfs = makeVromKeyframes();
    CHECK(EvaluateVolumeAnimation(kfs, 1, 360, "loop", 100) == 0.0f);
}

TEST_CASE("vrom: loops back — peak at t=388 (360+28)") {
    auto kfs = makeVromKeyframes();
    float v = EvaluateVolumeAnimation(kfs, 1, 360, "loop", 388);
    CHECK(v == doctest::Approx(0.6f));
}

TEST_CASE("single mode clamps at end") {
    std::vector<VolumeAnimKeyframe> kfs = {{ 0, 0 }, { 10, 1.0f }};
    // At t=20s with 1fps/10length, maxTime=10s, single clamps to 10s → frame 10 → value 1.0
    CHECK(EvaluateVolumeAnimation(kfs, 1, 10, "single", 20) == doctest::Approx(1.0f));
}

TEST_CASE("fps scaling: 30fps animation") {
    std::vector<VolumeAnimKeyframe> kfs = {{ 0, 0 }, { 30, 1.0f }};
    // At t=0.5s with 30fps → frame 15 → 50% of 0→1 = 0.5
    float v = EvaluateVolumeAnimation(kfs, 30, 60, "loop", 0.5);
    CHECK(v == doctest::Approx(0.5f));
}

TEST_CASE("negative time wraps correctly in loop mode") {
    auto kfs = makeVromKeyframes();
    float v = EvaluateVolumeAnimation(kfs, 1, 360, "loop", -1);
    // -1 mod 360 = 359 → frame 359 → after last keyframe (32) → 0
    CHECK(v == 0.0f);
}

TEST_CASE("REGRESSION: scene-time delta keeps anim locked to visuals") {
    // Bug follow-up: volume anim used a fixed 0.033s increment per eval, drifting
    // ahead of visuals when the render loop throttled to <30fps. Symptom: train
    // sound played before the train appeared. Fix: derive delta from the scene
    // clock (same clock visuals use), so if the render stalls the anim stalls too.
    //
    // This test models the delta-driven advance: given a sequence of scene-time
    // samples (possibly with gaps when the render stalled), the anim.time must
    // track the scene clock exactly, not the wall-clock eval cadence.
    struct AnimState {
        double time { 0 };
        double lastSceneTime { -1.0 };
        bool   playing { true };
    };
    auto advance = [](AnimState& s, double sceneTime) {
        if (s.lastSceneTime < 0.0) s.lastSceneTime = sceneTime;
        double delta      = sceneTime - s.lastSceneTime;
        if (delta < 0.0) delta = 0.0;
        s.lastSceneTime = sceneTime;
        if (s.playing) s.time += delta;
    };

    AnimState st;

    // First eval anchors lastSceneTime — does not advance. Subsequent samples
    // drive delta. This mirrors SceneBackend's lazy anchor on first PROPEVAL.
    advance(st, 0.0);
    CHECK(st.time == 0.0);

    // Render at 30Hz for 1 second — anim tracks scene clock exactly.
    for (int i = 1; i <= 30; i++) advance(st, i / 30.0);
    CHECK(st.time == doctest::Approx(1.0));

    // Render stalls (no time advance) — anim must NOT advance on eval
    double stallTime = st.lastSceneTime;
    for (int i = 0; i < 10; i++) advance(st, stallTime);
    CHECK(st.time == doctest::Approx(1.0));

    // Render resumes at 2.0s — anim jumps to match (delta = 1.0)
    advance(st, 2.0);
    CHECK(st.time == doctest::Approx(2.0));

    // Pause flag stops accumulation
    st.playing = false;
    advance(st, 3.0);
    CHECK(st.time == doctest::Approx(2.0));

    // Resume — further advances accumulate only from this point
    st.playing = true;
    advance(st, 4.0);
    CHECK(st.time == doctest::Approx(3.0));

    // Scene reload resets clock to 0 — guard against negative delta
    advance(st, 0.0);
    CHECK(st.time == doctest::Approx(3.0));
}

TEST_CASE("REGRESSION: anim-at-t0 diverges from static volume (trainsound init-sync)") {
    // Bug from wallpaper 2477602742: trainsound had static volume=0.5 but a "vrom"
    // animation that evaluates to 0.0 at t=0. The stream was initialized to 0.5 while
    // script-side currentVolume was initialized to anim.evaluate(0) = 0.0. On first
    // eval, the delta-threshold check (|newVol - currentVolume| < 0.001) skipped
    // updateSoundVolume() because both sides already read 0.0 — leaving the stream
    // stuck at 0.5 (audibly looping) until the animation diverged at t=26.8s.
    //
    // Fix: force-sync the stream to anim.evaluate(0) at compile time.
    // This test documents the invariant that drove the fix.
    auto        kfs                = makeVromKeyframes();
    const float staticSceneVolume  = 0.5f;
    const float animAtT0           = EvaluateVolumeAnimation(kfs, 1, 360, "loop", 0);

    CHECK(animAtT0 == 0.0f);
    CHECK(animAtT0 != staticSceneVolume);

    // Divergence window: how long before the anim first exceeds the threshold.
    // Long windows (>20s) make the bug blatantly audible; the sync must happen on init.
    float firstAudibleT = -1.0f;
    for (float t = 0.0f; t < 60.0f; t += 0.1f) {
        if (EvaluateVolumeAnimation(kfs, 1, 360, "loop", t) > 0.001f) {
            firstAudibleT = t;
            break;
        }
    }
    CHECK(firstAudibleT > 20.0f);
}

} // TEST_SUITE
