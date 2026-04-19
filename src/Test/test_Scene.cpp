#include <doctest.h>

#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"

#include <memory>

using namespace wallpaper;

TEST_SUITE("Scene") {

TEST_CASE("PassFrameTime accumulates elapsingTime and sets frameTime") {
    Scene s;
    CHECK(s.frameTime == doctest::Approx(0.0));
    CHECK(s.elapsingTime == doctest::Approx(0.0));
    s.PassFrameTime(0.016);
    CHECK(s.frameTime == doctest::Approx(0.016));
    CHECK(s.elapsingTime == doctest::Approx(0.016));
    s.PassFrameTime(0.032);
    CHECK(s.frameTime == doctest::Approx(0.032));
    CHECK(s.elapsingTime == doctest::Approx(0.048));
}

TEST_CASE("UpdateLinkedCamera with no-match name is a no-op") {
    Scene s;
    s.UpdateLinkedCamera("missing"); // neither linkedCameras nor cameras populated
    CHECK(true); // no crash
}

TEST_CASE("UpdateLinkedCamera clones the source camera's width/height to followers") {
    Scene s;

    auto src = std::make_shared<SceneCamera>(1920, 1080, 0.1f, 100.0f);
    s.cameras["main"] = src;

    auto f1 = std::make_shared<SceneCamera>(640, 480, 0.1f, 100.0f);
    auto f2 = std::make_shared<SceneCamera>(320, 240, 0.1f, 100.0f);
    s.cameras["follower1"] = f1;
    s.cameras["follower2"] = f2;
    s.linkedCameras["main"] = { "follower1", "follower2", "missing_follower" };

    s.UpdateLinkedCamera("main");
    // Clone() copies width/height; followers now match the source.
    CHECK(f1->Width() == doctest::Approx(1920.0));
    CHECK(f1->Height() == doctest::Approx(1080.0));
    CHECK(f2->Width() == doctest::Approx(1920.0));
    CHECK(f2->Height() == doctest::Approx(1080.0));
}

} // Scene
