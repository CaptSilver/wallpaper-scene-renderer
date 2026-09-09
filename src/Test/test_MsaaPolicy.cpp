#include <doctest.h>

#include "Scene/Scene.h"

using wallpaper::Scene;

// The sample count is not a per-frame cost, it is a per-pass one: every pass
// that writes _rt_default resolves a full-screen multisampled buffer.  A scene
// that draws a couple of layers can afford 4x at any resolution; one that draws
// 206 of them cannot afford it at 4K.  autoMsaaSamples keeps the request while
// (output pixels x passes) fits the budget and turns MSAA off past it — the
// measured cost is a cliff, not a slope, so there is no middle count to pick.
TEST_SUITE("MsaaPolicy") {
    // 3705485676 "ABRAR KHAN | 4K" compiles to 206 graph nodes (151 of them
    // write _rt_default).  autoMsaaSamples is called with the node count.
    constexpr wallpaper::u32 kHeavyPasses = 206;

    TEST_CASE("a scene that draws few layers keeps 4x even at 4K") {
        CHECK(Scene::autoMsaaSamples(4, 3840, 2160, 20) == 4);
    }

    TEST_CASE("a 206-pass scene keeps 4x at 1080p and 1440p") {
        CHECK(Scene::autoMsaaSamples(4, 1920, 1080, kHeavyPasses) == 4);
        CHECK(Scene::autoMsaaSamples(4, 2560, 1440, kHeavyPasses) == 4);
    }

    TEST_CASE("the same scene drops to 1x at 4K, where 4x measured 19 fps") {
        CHECK(Scene::autoMsaaSamples(4, 3840, 2160, kHeavyPasses) == 1);
    }

    TEST_CASE("there is no useful middle sample count — the policy is on or off") {
        // Measured on an RX 9070 XT at 3840x2160 with 206 passes, uncapped:
        // 1x = 125 fps, 2x = 29 fps, 4x = 19 fps.  Halving the sample count
        // from 4 buys 10 fps and still misses 60, so stepping down to 2x would
        // spend edge quality and stay unplayable.  Above the budget, MSAA goes
        // off; below it, the request is honoured as-is.
        CHECK(Scene::autoMsaaSamples(4, 3840, 2160, 100) == 4);
        CHECK(Scene::autoMsaaSamples(4, 3840, 2160, 206) == 1);
    }

    TEST_CASE("MSAA already off stays off") {
        CHECK(Scene::autoMsaaSamples(1, 3840, 2160, kHeavyPasses) == 1);
        CHECK(Scene::autoMsaaSamples(0, 3840, 2160, kHeavyPasses) == 1);
    }

    TEST_CASE("it never raises the requested sample count") {
        CHECK(Scene::autoMsaaSamples(2, 640, 360, 4) == 2);
    }

    TEST_CASE("resolving twice does not ratchet the sample count down") {
        // The graph is recompiled on resolution change.  Resolving in place
        // would make the 4K answer permanent, so the scene keeps what it asked
        // for in msaaRequested and every compile resolves from that.
        Scene scene;
        scene.msaaRequested = 4;

        scene.msaaSamples = Scene::autoMsaaSamples(scene.msaaRequested, 3840, 2160, kHeavyPasses);
        CHECK(scene.msaaSamples == 1);

        scene.msaaSamples = Scene::autoMsaaSamples(scene.msaaRequested, 2560, 1440, kHeavyPasses);
        CHECK(scene.msaaSamples == 4);
    }

    TEST_CASE("an unknown extent or pass count leaves the request alone") {
        // Called before the swapchain extent or the graph is known: estimating
        // from zero would silently disable MSAA for every scene.
        CHECK(Scene::autoMsaaSamples(4, 3840, 2160, 0) == 4);
        CHECK(Scene::autoMsaaSamples(4, 0, 0, kHeavyPasses) == 4);
    }
}
