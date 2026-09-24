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

    TEST_CASE("a pin with only one axis set falls through to the scale path") {
        // Both pin_w and pin_h must be positive to win -- a caller that only
        // half-pins (or leaves the other axis at its 0 default) gets the
        // scaled size on that call, not a pin clamped against a zero.
        auto zeroPinW = wallpaper::ResolveRenderExtent(3840, 2160, 1.0, 0, 720);
        CHECK(zeroPinW.width == 3840u);
        CHECK(zeroPinW.height == 2160u);

        auto zeroPinH = wallpaper::ResolveRenderExtent(3840, 2160, 1.0, 1280, 0);
        CHECK(zeroPinH.width == 3840u);
        CHECK(zeroPinH.height == 2160u);
    }

    TEST_CASE("a pin below the floor is still floored") {
        // The standalone viewer's -R pins an exact size (SceneObject's
        // renderPixelWidth/Height); the pin used to bypass the floor
        // outright, so a small -R handed the renderer a target too small
        // for the fixed HDR bloom chain's deepest mip to round to a
        // nonzero size, and the pass silently bailed.
        auto e = wallpaper::ResolveRenderExtent(1920, 1080, 1.0, 8, 8);
        CHECK(e.width >= wallpaper::kMinRenderExtent);
        CHECK(e.height >= wallpaper::kMinRenderExtent);
    }

    TEST_CASE("the floor survives the deepest HDR bloom mip on either path") {
        // buildBloomAndReflection's fixed mip chain (WPSceneParser.cpp)
        // bottoms out at WE_BLOOM_MIP4, scale 1/16. If kMinRenderExtent
        // ever drops below 16, or a future bloom-knob rewire deepens the
        // chain past what the floor covers, this is the tripwire -- on
        // both the render-scale path and the standalone-viewer pin path.
        constexpr float kDeepestBloomMipScale = 0.0625f; // WE_BLOOM_MIP4
        auto            scaled = wallpaper::ResolveRenderExtent(1, 1, wallpaper::kMinRenderScale);
        auto            pinned = wallpaper::ResolveRenderExtent(1920, 1080, 1.0, 8, 8);
        CHECK((uint32_t)((float)scaled.width * kDeepestBloomMipScale) >= 1u);
        CHECK((uint32_t)((float)scaled.height * kDeepestBloomMipScale) >= 1u);
        CHECK((uint32_t)((float)pinned.width * kDeepestBloomMipScale) >= 1u);
        CHECK((uint32_t)((float)pinned.height * kDeepestBloomMipScale) >= 1u);
    }
}
