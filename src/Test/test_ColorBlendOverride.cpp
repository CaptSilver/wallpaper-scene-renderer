#include <doctest.h>

#include "ColorBlendOverride.hpp"

using namespace wallpaper;

TEST_SUITE("ColorBlendOverride") {
    TEST_CASE("a screen layer with effects draws premultiplied colour into its chain") {
        // Screen ignores alpha, so the chain must not carry colour where the
        // image is transparent; its alpha stays as the old base draw left it.
        auto b = SplitColorBlendOverride(BlendMode::Translucent, BlendMode::Opaque, true);
        CHECK(b.base == BlendMode::PremultipliedColorOnly);
        CHECK(b.final == BlendMode::Opaque);
    }

    TEST_CASE("without effects the base draw is the final step and takes the override") {
        auto b = SplitColorBlendOverride(BlendMode::Translucent, BlendMode::Opaque, false);
        CHECK(b.base == BlendMode::Opaque);
        CHECK(b.final == BlendMode::Opaque);
    }

    TEST_CASE("no override leaves both steps on the material's blend") {
        auto b = SplitColorBlendOverride(BlendMode::Additive, BlendMode::Disable, true);
        CHECK(b.base == BlendMode::Additive);
        CHECK(b.final == BlendMode::Additive);
    }

    TEST_CASE("an additive override keeps applying to both steps") {
        // SRC_ALPHA already scales colour by alpha on the base draw.
        auto b = SplitColorBlendOverride(BlendMode::Translucent, BlendMode::Additive, true);
        CHECK(b.base == BlendMode::Additive);
        CHECK(b.final == BlendMode::Additive);
    }
}
