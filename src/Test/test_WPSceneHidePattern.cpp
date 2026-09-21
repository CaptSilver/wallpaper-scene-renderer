#include <doctest.h>

#include "WPSceneHidePattern.hpp"

// Pure predicate behind sceneviewer's --hide-pattern flag: does an object's
// name contain any of the comma-separated needles?  Pinned separately from
// the parser wiring (test_WPSceneParse.cpp) so the match rules stay correct
// on their own, independent of which object kind calls it.

using wallpaper::HidePatternMatches;

TEST_SUITE("HidePatternMatches") {
    TEST_CASE("empty pattern never matches") { CHECK_FALSE(HidePatternMatches("", "anything")); }

    TEST_CASE("case-insensitive substring match") { CHECK(HidePatternMatches("HAIR", "hair_c1")); }

    TEST_CASE("comma-separated needles, matches any") {
        CHECK(HidePatternMatches("xyz,hair,qrs", "my_hair_layer"));
        CHECK_FALSE(HidePatternMatches("xyz,qrs", "my_hair_layer"));
    }

    TEST_CASE("empty needle segment (leading/trailing/double comma) never matches everything") {
        CHECK_FALSE(HidePatternMatches(",hair", "unrelated"));       // leading comma
        CHECK_FALSE(HidePatternMatches("hair,", "unrelated"));       // trailing comma
        CHECK_FALSE(HidePatternMatches("hair,,other", "unrelated")); // double comma
    }
}
