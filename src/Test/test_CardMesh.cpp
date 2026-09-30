#include <doctest.h>

#include "CardMesh.hpp"

#include <utility>

using wallpaper::CardStrip;
using wallpaper::CardStripFacing;
using wallpaper::CardVertex;

// A layer card and a model mesh share one global front-face rule, so they have
// to agree on which way a camera-facing triangle turns.  When they disagree,
// every material that asks to cull back faces drops one of the two — a puppet
// layer whose effect chain composites through cards then vanishes whichever
// direction the culling goes, because each direction kills a different link.
TEST_SUITE("card mesh winding") {
    TEST_CASE("a camera-facing card turns counter-clockwise") {
        const auto card = CardStrip(-100.0f, 100.0f, -50.0f, 50.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        CHECK(CardStripFacing(card) > 0.0f);
    }

    TEST_CASE("winding does not depend on the card's aspect or offset") {
        const auto wide = CardStrip(-1000.0f, 1000.0f, -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        const auto tall = CardStrip(-1.0f, 1.0f, -1000.0f, 1000.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        const auto off  = CardStrip(300.0f, 500.0f, 700.0f, 900.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        CHECK(CardStripFacing(wide) > 0.0f);
        CHECK(CardStripFacing(tall) > 0.0f);
        CHECK(CardStripFacing(off) > 0.0f);
    }

    // Reordering the strip to fix the winding must carry each corner's
    // texcoord with it, or the layer renders mirrored instead of missing.
    TEST_CASE("each corner keeps its own texcoord") {
        const auto card = CardStrip(-2.0f, 2.0f, -1.0f, 1.0f, 0.1f, 0.9f, 0.2f, 0.8f);
        for (const CardVertex& v : card) {
            const bool is_left = v.position[0] == -2.0f;
            const bool is_top  = v.position[1] == 1.0f;
            CHECK(v.texcoord[0] == doctest::Approx(is_left ? 0.1f : 0.9f));
            CHECK(v.texcoord[1] == doctest::Approx(is_top ? 0.2f : 0.8f));
        }
    }

    // Both card generators hand SceneVertexArray parallel arrays, so the split
    // has to keep vertex order — a shuffled flatten would reintroduce exactly
    // the winding bug this file guards against.
    TEST_CASE("flattening preserves vertex order across both arrays") {
        const auto card = CardStrip(-2.0f, 2.0f, -1.0f, 1.0f, 0.1f, 0.9f, 0.2f, 0.8f);
        const auto flat = wallpaper::FlattenCardStrip(card);
        for (std::size_t i = 0; i < card.size(); i++) {
            CHECK(flat.position[i * 3 + 0] == doctest::Approx(card[i].position[0]));
            CHECK(flat.position[i * 3 + 1] == doctest::Approx(card[i].position[1]));
            CHECK(flat.position[i * 3 + 2] == doctest::Approx(card[i].position[2]));
            CHECK(flat.texcoord[i * 2 + 0] == doctest::Approx(card[i].texcoord[0]));
            CHECK(flat.texcoord[i * 2 + 1] == doctest::Approx(card[i].texcoord[1]));
        }
    }

    TEST_CASE("the strip covers all four corners exactly once") {
        const auto card = CardStrip(-2.0f, 2.0f, -1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
        int        left = 0, right = 0, bottom = 0, top = 0;
        for (const CardVertex& v : card) {
            v.position[0] == -2.0f ? left++ : right++;
            v.position[1] == -1.0f ? bottom++ : top++;
        }
        CHECK(left == 2);
        CHECK(right == 2);
        CHECK(bottom == 2);
        CHECK(top == 2);
    }
}

using wallpaper::PaddedTextureUvRate;

// A 1920x1080 image stored in a 2048x2048 texture: the card must only sample
// the image part, whatever the model's `nopadding` flag says.  Before the fix
// the parser skipped this for `nopadding` models and the image filled only the
// top 53% of the screen (1080 / 2048).
TEST_SUITE("padded texture uv rate") {
    TEST_CASE("a padded texture maps the card onto the image part") {
        const auto rate = PaddedTextureUvRate(2048.0f, 2048.0f, 1920.0f, 1080.0f);
        CHECK(rate[0] == doctest::Approx(1920.0f / 2048.0f));
        CHECK(rate[1] == doctest::Approx(1080.0f / 2048.0f));
    }

    TEST_CASE("an unpadded texture maps one to one") {
        const auto rate = PaddedTextureUvRate(1920.0f, 1080.0f, 1920.0f, 1080.0f);
        CHECK(rate[0] == doctest::Approx(1.0f));
        CHECK(rate[1] == doctest::Approx(1.0f));
    }

    // Each side on its own: a zero width or a zero height alone must not
    // divide by zero either.
    TEST_CASE("a missing texture size falls back to one to one") {
        for (const auto& [w, h] : { std::pair { 0.0f, 0.0f },
                                    std::pair { 0.0f, 2048.0f },
                                    std::pair { 2048.0f, 0.0f } }) {
            CAPTURE(w);
            CAPTURE(h);
            const auto rate = PaddedTextureUvRate(w, h, 1920.0f, 1080.0f);
            CHECK(rate[0] == doctest::Approx(1.0f));
            CHECK(rate[1] == doctest::Approx(1.0f));
        }
    }
}
