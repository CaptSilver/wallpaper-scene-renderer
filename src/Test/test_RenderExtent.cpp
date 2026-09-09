#include <doctest.h>

#include "Scene/RenderExtent.h"

// Rendering below the display's native size and letting Qt upscale is the
// other big lever on a wallpaper that is too heavy: nearly everything the
// renderer does is per-pixel and per-pass, so halving each axis quarters it.
TEST_SUITE("RenderExtent") {
    TEST_CASE("native scale renders at the item's device pixels") {
        auto e = wallpaper::ResolveRenderExtent(3840, 2160, 1.0);
        CHECK(e.width == 3840u);
        CHECK(e.height == 2160u);
    }

    TEST_CASE("half scale quarters the pixels") {
        auto e = wallpaper::ResolveRenderExtent(3840, 2160, 0.5);
        CHECK(e.width == 1920u);
        CHECK(e.height == 1080u);
    }

    TEST_CASE("odd sizes round rather than truncate") {
        // 1365.5 must not become 1365 on one axis and 767 on the other; a
        // half-pixel bias compounds into a visibly soft upscale.
        auto e = wallpaper::ResolveRenderExtent(2731, 1535, 0.5);
        CHECK(e.width == 1366u);
        CHECK(e.height == 768u);
    }

    TEST_CASE("the scale is clamped at both ends") {
        CHECK(wallpaper::ResolveRenderExtent(1920, 1080, 0.05).width ==
              wallpaper::ResolveRenderExtent(1920, 1080, wallpaper::kMinRenderScale).width);
        CHECK(wallpaper::ResolveRenderExtent(1920, 1080, 4.0).width == 1920u);
    }

    TEST_CASE("a tiny item never resolves to nothing") {
        // A zero-sized target is a broken swapchain, not a cheap render.
        auto e = wallpaper::ResolveRenderExtent(80, 40, 0.25);
        CHECK(e.width >= wallpaper::kMinRenderExtent);
        CHECK(e.height >= wallpaper::kMinRenderExtent);
    }

    TEST_CASE("an explicit pin beats the scale") {
        // The standalone viewer's -R has already decided the exact physical
        // size, because Wayland fractional scaling makes dpr unreliable at
        // window-show time.
        auto e = wallpaper::ResolveRenderExtent(3840, 2160, 0.5, 1280, 720);
        CHECK(e.width == 1280u);
        CHECK(e.height == 720u);
    }
}
