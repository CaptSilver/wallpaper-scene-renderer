#include "doctest.h"

#include <string>
#include <vector>

#include "Scene/SceneLoadFailure.hpp"

using namespace wallpaper;

// A scene that fails to load leaves a black desktop.  The renderer knows
// exactly why -- the package directory would not mount, no scene JSON was
// found, the JSON was malformed -- and the point of these strings is that the
// reason reaching the user names the thing that went wrong instead of the
// generic "nothing drew" the watchdog falls back to.
TEST_SUITE("scene load failure reasons") {
    TEST_CASE("each reason names the path or id that failed") {
        const auto mount = sceneLoadFailureReason(SceneLoadFailure::PkgDirUnmountable,
                                                  "/home/u/steamapps/431960/12345");
        CHECK(mount.find("/home/u/steamapps/431960/12345") != std::string::npos);

        const auto missing =
            sceneLoadFailureReason(SceneLoadFailure::NoSceneJson, "/home/u/pkgdir");
        CHECK(missing.find("/home/u/pkgdir") != std::string::npos);

        const auto malformed =
            sceneLoadFailureReason(SceneLoadFailure::SceneJsonMalformed, "998877");
        CHECK(malformed.find("998877") != std::string::npos);
    }

    TEST_CASE("the three reasons are distinguishable from one another") {
        const auto a = sceneLoadFailureReason(SceneLoadFailure::PkgDirUnmountable, "x");
        const auto b = sceneLoadFailureReason(SceneLoadFailure::NoSceneJson, "x");
        const auto c = sceneLoadFailureReason(SceneLoadFailure::SceneJsonMalformed, "x");
        CHECK(a != b);
        CHECK(b != c);
        CHECK(a != c);
    }

    TEST_CASE("dispatch is a no-op when nothing is listening") {
        std::function<void(const std::string&)> none;
        CHECK_NOTHROW(dispatchSceneLoadFailure(none, SceneLoadFailure::NoSceneJson, "p"));
    }

    TEST_CASE("dispatch delivers the reason exactly once") {
        std::vector<std::string>                got;
        std::function<void(const std::string&)> cb = [&got](const std::string& r) {
            got.push_back(r);
        };
        dispatchSceneLoadFailure(cb, SceneLoadFailure::SceneJsonMalformed, "42");
        REQUIRE(got.size() == 1);
        CHECK(got[0].find("42") != std::string::npos);
    }
}
