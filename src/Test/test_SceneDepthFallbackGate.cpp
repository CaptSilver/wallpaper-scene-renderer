#include <doctest.h>

#include "SpecTexs.hpp"

namespace {
bool ShouldScheduleDepthResolve(bool d32_sampleable, bool has_volumetric_light) {
    return ! d32_sampleable && has_volumetric_light;
}
} // namespace

TEST_SUITE("SceneDepthFallbackGate") {
    TEST_CASE("path A taken when d32_sampleable=true (no resolve)") {
        CHECK(ShouldScheduleDepthResolve(true, false) == false);
        CHECK(ShouldScheduleDepthResolve(true, true)  == false);
    }
    TEST_CASE("path D taken when d32_sampleable=false AND volumetric light present") {
        CHECK(ShouldScheduleDepthResolve(false, true) == true);
    }
    TEST_CASE("no resolve when no volumetric light, even on non-sampleable device") {
        CHECK(ShouldScheduleDepthResolve(false, false) == false);
    }
    TEST_CASE("output key contract") {
        CHECK(wallpaper::WE_SCENE_DEPTH_LINEAR == "_rt_sceneDepthLinear");
    }
}
