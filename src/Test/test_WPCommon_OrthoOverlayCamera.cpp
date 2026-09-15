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
