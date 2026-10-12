#include <doctest.h>
#include "WPCommon.hpp"

using namespace wallpaper;

// The predicate that picks the ortho overlay camera ("global_ortho") for a
// flat layer's final composite instead of the scene's real camera.  Pulled out
// of WPSceneParser.cpp so the effect chain's final-camera pick, the no-effect
// flat-layer pick, and a 3D compose layer's camera-name choice all read the
// same three-way AND instead of three near-identical copies of it.
TEST_SUITE("WPCommon.UsesOrthoOverlayCamera") {
    TEST_CASE("no ortho overlay camera in the scene -> false regardless of other inputs") {
        CHECK_FALSE(UsesOrthoOverlayCamera(false, false, false));
        CHECK_FALSE(UsesOrthoOverlayCamera(false, false, true));
        CHECK_FALSE(UsesOrthoOverlayCamera(false, true, false));
        CHECK_FALSE(UsesOrthoOverlayCamera(false, true, true));
    }

    TEST_CASE("overlay camera present, layer flat, no model ancestor -> true") {
        CHECK(UsesOrthoOverlayCamera(true, false, false));
    }

    TEST_CASE("overlay camera present but layer declares its own perspective -> false") {
        CHECK_FALSE(UsesOrthoOverlayCamera(true, true, false));
    }

    TEST_CASE("overlay camera present but layer inherits a 3D model's transform -> false") {
        CHECK_FALSE(UsesOrthoOverlayCamera(true, false, true));
    }

    TEST_CASE("overlay camera present, perspective layer that also inherits model space -> false") {
        CHECK_FALSE(UsesOrthoOverlayCamera(true, true, true));
    }
}

TEST_SUITE("FinalCompositeCamera") {
    TEST_CASE("a fullscreen layer composites through the 2x2 effect camera") {
        // Its final quad is the unit 2x2 card at the origin; only the effect
        // camera maps that onto the whole target.
        CHECK(FinalCompositeCamera(true, true, false, false) == "effect");
        CHECK(FinalCompositeCamera(true, false, false, false) == "effect");
    }

    TEST_CASE("a flat layer in a perspective scene uses the ortho overlay") {
        CHECK(FinalCompositeCamera(false, true, false, false) == "global_ortho");
    }

    TEST_CASE("otherwise the layer keeps the scene's active camera") {
        CHECK(FinalCompositeCamera(false, false, false, false).empty());
        CHECK(FinalCompositeCamera(false, true, true, false).empty());
        CHECK(FinalCompositeCamera(false, true, false, true).empty());
    }
}

TEST_SUITE("InheritsParentTransform") {
    TEST_CASE("an on-screen layer with a parent group inherits its transform") {
        CHECK(InheritsParentTransform(false, false, true));
    }

    TEST_CASE("an offscreen dependency stays centred in its own target") {
        CHECK_FALSE(InheritsParentTransform(true, false, true));
    }

    TEST_CASE("a fullscreen layer ignores its group: it covers the target as is") {
        CHECK_FALSE(InheritsParentTransform(false, true, true));
    }

    TEST_CASE("no parent, nothing to inherit") {
        CHECK_FALSE(InheritsParentTransform(false, false, false));
    }
}
