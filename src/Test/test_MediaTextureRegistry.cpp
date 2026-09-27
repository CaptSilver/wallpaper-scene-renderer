#include <doctest.h>

#include "Scene/include/Scene/MediaTextureRegistry.hpp"

using namespace wallpaper;

TEST_SUITE("MediaTextureRegistry") {
    TEST_CASE("empty registry resolves nothing") {
        MediaTextureRegistry reg;
        CHECK_FALSE(reg.get("$mediaThumbnail").has_value());
    }

    TEST_CASE("reset then get returns the right entry; unrelated key is nullopt") {
        MediaTextureRegistry reg;
        reg.reset({
            MediaTextureSlot { "$mediaThumbnail", 96, 48, {}, nullptr },
        });
        auto got = reg.get("$mediaThumbnail");
        REQUIRE(got.has_value());
        CHECK(got->width == 96);
        CHECK(got->height == 48);
        CHECK_FALSE(reg.get("$mediaPreviousThumbnail").has_value());
    }

    TEST_CASE("a second reset replaces, not merges") {
        MediaTextureRegistry reg;
        reg.reset({
            MediaTextureSlot { "$mediaThumbnail", 96, 48, {}, nullptr },
        });
        // A wallpaper reload with a scene that has NO media slots at all --
        // the previous scene's entry must stop resolving.
        reg.reset({});
        CHECK_FALSE(reg.get("$mediaThumbnail").has_value());
    }

    TEST_CASE("a second reset with a different slot set replaces the first entirely") {
        MediaTextureRegistry reg;
        reg.reset({
            MediaTextureSlot { "$mediaThumbnail", 96, 48, {}, nullptr },
        });
        reg.reset({
            MediaTextureSlot { "$mediaPreviousThumbnail", 64, 64, {}, nullptr },
        });
        CHECK_FALSE(reg.get("$mediaThumbnail").has_value());
        auto got = reg.get("$mediaPreviousThumbnail");
        REQUIRE(got.has_value());
        CHECK(got->width == 64);
    }
}
