#include <doctest.h>

#include "Scene/Scene.h"
#include "Scene/ConcurrentRenderers.h"

using wallpaper::Scene;

// The sample count is not a per-frame cost, it is a per-pass one: every pass
// that writes _rt_default resolves a full-screen multisampled buffer, so the
// cost scales with output pixels and with how many layers the scene draws.
// autoMsaaSamples caps the request by an output-resolution tier (4x up to
// 1080p-class, 2x up to 1440p-class, 1x beyond) and then by a pass-count
// budget that can lower it further.  The tier is keyed on PIXEL COUNT rather
// than on width, height or a named mode, so ultrawide and portrait panels land
// on the tier their actual resolve cost deserves.
TEST_SUITE("MsaaPolicy") {
    // 3705485676 "ABRAR KHAN | 4K" compiles to 206 graph nodes (151 of them
    // write _rt_default).  autoMsaaSamples is called with the node count.
    constexpr wallpaper::u32 kHeavyPasses = 206;

    TEST_CASE("the tier follows pixel count, so 16:9 lands where expected") {
        CHECK(Scene::autoMsaaSamples(4, 1920, 1080, kHeavyPasses) == 4);
        CHECK(Scene::autoMsaaSamples(4, 2560, 1440, kHeavyPasses) == 2);
        CHECK(Scene::autoMsaaSamples(4, 3840, 2160, kHeavyPasses) == 1);
    }

    TEST_CASE("ultrawides tier by area, not by height or by name") {
        // A 1440-tall ultrawide is not a 1440p monitor: 3440x1440 carries 34%
        // more pixels than 2560x1440, and 5120x1440 carries nearly as many as
        // 4K.  Keying the tier off height would hand all three the same
        // sample count and let the widest one fall over.
        CHECK(Scene::autoMsaaSamples(4, 2560, 1080, kHeavyPasses) == 4);  // 2.76 M
        CHECK(Scene::autoMsaaSamples(4, 3440, 1440, kHeavyPasses) == 2);  // 4.95 M
        CHECK(Scene::autoMsaaSamples(4, 3840, 1600, kHeavyPasses) == 2);  // 6.14 M
        CHECK(Scene::autoMsaaSamples(4, 5120, 1440, kHeavyPasses) == 1);  // 7.37 M
    }

    TEST_CASE("orientation does not change the answer") {
        // A rotated panel is the same number of pixels to resolve.
        CHECK(Scene::autoMsaaSamples(4, 1440, 2560, kHeavyPasses) ==
              Scene::autoMsaaSamples(4, 2560, 1440, kHeavyPasses));
        CHECK(Scene::autoMsaaSamples(4, 2160, 3840, kHeavyPasses) ==
              Scene::autoMsaaSamples(4, 3840, 2160, kHeavyPasses));
    }

    TEST_CASE("the tier boundaries are inclusive at the exact pixel count") {
        // A display landing exactly on a threshold belongs to the better tier.
        // Tested at the edge because a comparison that slips from <= to < only
        // misbehaves for the single resolution that sits on the line, and every
        // other case in this suite sits comfortably inside a tier.
        CHECK(Scene::autoMsaaSamples(4, 2000, 1500, 4) == 4);   // exactly 3.0 M
        CHECK(Scene::autoMsaaSamples(4, 2000, 1501, 4) == 2);   // one row over
        CHECK(Scene::autoMsaaSamples(4, 2600, 2500, 4) == 2);   // exactly 6.5 M
        CHECK(Scene::autoMsaaSamples(4, 2600, 2501, 4) == 1);   // one row over
    }

    TEST_CASE("the concurrent term is counted at the boundary too") {
        // Two screens summing exactly to a threshold stay in the better tier.
        CHECK(Scene::autoMsaaSamples(4, 1000, 1500, 4, 1500000ull) == 4);
        CHECK(Scene::autoMsaaSamples(4, 1000, 1500, 4, 1500001ull) == 2);
    }

    TEST_CASE("a pathological pass count drops MSAA even on a small display") {
        // The tier is a ceiling, not a promise: the sample count multiplies
        // with the number of layers, so a scene that draws hundreds of them
        // can exhaust the budget at a resolution the tier would have allowed.
        CHECK(Scene::autoMsaaSamples(4, 1920, 1080, 600) == 1);
    }

    TEST_CASE("a light scene is still capped by the tier") {
        // Measured cost is a cliff: at 4K the heavy scene runs 19 fps at 4x
        // and still only 29 fps at 2x, so 4K gets one sample regardless of how
        // little the scene draws.
        CHECK(Scene::autoMsaaSamples(4, 3840, 2160, 4) == 1);
        CHECK(Scene::autoMsaaSamples(4, 1920, 1080, 4) == 4);
    }

    TEST_CASE("MSAA already off stays off") {
        CHECK(Scene::autoMsaaSamples(1, 1920, 1080, kHeavyPasses) == 1);
        CHECK(Scene::autoMsaaSamples(0, 1920, 1080, kHeavyPasses) == 1);
    }

    TEST_CASE("it never raises the requested sample count") {
        CHECK(Scene::autoMsaaSamples(2, 640, 360, 4) == 2);
        CHECK(Scene::autoMsaaSamples(2, 1920, 1080, kHeavyPasses) == 2);
    }

    TEST_CASE("resolving twice does not ratchet the sample count down") {
        // The graph is recompiled on resolution change.  Resolving in place
        // would make the 4K answer permanent, so the scene keeps what it asked
        // for in msaaRequested and every compile resolves from that.
        Scene scene;
        scene.msaaRequested = 4;

        scene.msaaSamples = Scene::autoMsaaSamples(scene.msaaRequested, 3840, 2160, kHeavyPasses);
        CHECK(scene.msaaSamples == 1);

        scene.msaaSamples = Scene::autoMsaaSamples(scene.msaaRequested, 1920, 1080, kHeavyPasses);
        CHECK(scene.msaaSamples == 4);
    }

    TEST_CASE("an unknown extent or pass count leaves the request alone") {
        // Called before the swapchain extent or the graph is known: estimating
        // from zero would silently disable MSAA for every scene.
        CHECK(Scene::autoMsaaSamples(4, 3840, 2160, 0) == 4);
        CHECK(Scene::autoMsaaSamples(4, 0, 0, kHeavyPasses) == 4);
    }

    TEST_CASE("only real sample counts are accepted") {
        // Guards the override paths: a value the pipeline was never built for
        // must be rejected outright rather than rounded to something plausible.
        CHECK(Scene::isSupportedMsaaSampleCount(1));
        CHECK(Scene::isSupportedMsaaSampleCount(2));
        CHECK(Scene::isSupportedMsaaSampleCount(4));
        CHECK(Scene::isSupportedMsaaSampleCount(8));
        CHECK_FALSE(Scene::isSupportedMsaaSampleCount(0));
        CHECK_FALSE(Scene::isSupportedMsaaSampleCount(3));
        CHECK_FALSE(Scene::isSupportedMsaaSampleCount(6));
        CHECK_FALSE(Scene::isSupportedMsaaSampleCount(16));
    }

    TEST_CASE("the user setting maps to a request and an auto flag") {
        // 0 is "let the policy decide"; anything else is the user pinning a
        // count, which must also switch the automatic tiering off or the tier
        // would immediately override the choice they just made.
        CHECK(Scene::msaaRequestFromMode(0, 4).samples == 4);
        CHECK(Scene::msaaRequestFromMode(0, 4).autoScale == true);

        CHECK(Scene::msaaRequestFromMode(1, 4).samples == 1);
        CHECK(Scene::msaaRequestFromMode(1, 4).autoScale == false);

        CHECK(Scene::msaaRequestFromMode(2, 4).samples == 2);
        CHECK(Scene::msaaRequestFromMode(2, 4).autoScale == false);

        CHECK(Scene::msaaRequestFromMode(4, 4).samples == 4);
        CHECK(Scene::msaaRequestFromMode(4, 4).autoScale == false);
    }

    TEST_CASE("auto mode carries the scene's own request through") {
        // A skybox scene has already been narrowed to 1 by the time the mode
        // is applied; auto must not resurrect MSAA the parser turned off.
        CHECK(Scene::msaaRequestFromMode(0, 1).samples == 1);
        CHECK(Scene::msaaRequestFromMode(0, 1).autoScale == true);
    }

    TEST_CASE("an unrecognised mode falls back to auto rather than to off") {
        // A stale or future config value must not silently disable MSAA.
        CHECK(Scene::msaaRequestFromMode(3, 4).samples == 4);
        CHECK(Scene::msaaRequestFromMode(3, 4).autoScale == true);
        CHECK(Scene::msaaRequestFromMode(-1, 4).autoScale == true);
        CHECK(Scene::msaaRequestFromMode(99, 4).autoScale == true);
    }

    TEST_CASE("a second monitor counts toward the tier") {
        // Every screen is its own wallpaper plasmoid with its own Vulkan
        // device, all on one GPU.  Two 1440p panels each concluding "2x is
        // affordable" in isolation would together push more pixels than the
        // 4K screen we measured at 19 fps, so the tier keys on everything this
        // process is drawing, not on the screen asking.
        constexpr wallpaper::u64 k1440p = 2560ull * 1440;
        constexpr wallpaper::u64 k1080p = 1920ull * 1080;

        CHECK(Scene::autoMsaaSamples(4, 2560, 1440, kHeavyPasses, 0) == 2);
        CHECK(Scene::autoMsaaSamples(4, 2560, 1440, kHeavyPasses, k1440p) == 1);

        // Two 1080p screens still fit the 2x tier.
        CHECK(Scene::autoMsaaSamples(4, 1920, 1080, kHeavyPasses, 0) == 4);
        CHECK(Scene::autoMsaaSamples(4, 1920, 1080, kHeavyPasses, k1080p) == 2);

        // Three of them do not.
        CHECK(Scene::autoMsaaSamples(4, 1920, 1080, kHeavyPasses, 2 * k1080p) == 2);
        CHECK(Scene::autoMsaaSamples(4, 1920, 1080, kHeavyPasses, 4 * k1080p) == 1);
    }

    TEST_CASE("a single screen is unaffected by the concurrent term") {
        // Default argument keeps every single-monitor caller on the old path.
        CHECK(Scene::autoMsaaSamples(4, 1920, 1080, kHeavyPasses, 0) ==
              Scene::autoMsaaSamples(4, 1920, 1080, kHeavyPasses));
    }
}

// Every wallpaper plasmoid builds its own renderer inside the one plasmashell
// process, and none of them can see the others.  This registry is how a
// renderer learns what the rest of the process is drawing, so the MSAA tier
// can be taken over the whole GPU's load instead of one screen's.
TEST_SUITE("ConcurrentRenderers") {
    TEST_CASE("an empty registry reports nothing") {
        wallpaper::ConcurrentRenderers reg;
        CHECK(reg.PixelsExcluding(&reg) == 0u);
    }

    TEST_CASE("a renderer never counts itself") {
        wallpaper::ConcurrentRenderers reg;
        int a = 0, b = 0;
        reg.Set(&a, 1000);
        reg.Set(&b, 500);
        CHECK(reg.PixelsExcluding(&a) == 500u);
        CHECK(reg.PixelsExcluding(&b) == 1000u);
    }

    TEST_CASE("a resolution change replaces the previous figure") {
        wallpaper::ConcurrentRenderers reg;
        int a = 0, b = 0;
        reg.Set(&a, 1000);
        reg.Set(&b, 500);
        reg.Set(&b, 2000);
        CHECK(reg.PixelsExcluding(&a) == 2000u);
    }

    TEST_CASE("a torn-down renderer stops counting") {
        // A screen unplug or a wallpaper switch destroys the renderer; leaving
        // its pixels behind would permanently depress the tier for whoever is
        // left.
        wallpaper::ConcurrentRenderers reg;
        int a = 0, b = 0;
        reg.Set(&a, 1000);
        reg.Set(&b, 500);
        reg.Remove(&b);
        CHECK(reg.PixelsExcluding(&a) == 0u);
    }

    TEST_CASE("removing an unknown renderer is harmless") {
        wallpaper::ConcurrentRenderers reg;
        int a = 0, ghost = 0;
        reg.Set(&a, 1000);
        reg.Remove(&ghost);
        CHECK(reg.PixelsExcluding(&ghost) == 1000u);
    }
}
