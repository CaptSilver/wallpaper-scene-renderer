#include <doctest.h>

#include "TexVariants.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::texvariant;

TEST_SUITE("TexVariants") {
    TEST_CASE("RegionBytes counts 4x4 blocks for BC formats") {
        CHECK(RegionBytes(TextureFormat::BC1, 8, 8) == 4 * 8);
        CHECK(RegionBytes(TextureFormat::BC2, 8, 8) == 4 * 16);
        CHECK(RegionBytes(TextureFormat::BC3, 8, 4) == 2 * 16);
        CHECK(RegionBytes(TextureFormat::BC7, 4, 4) == 16);
        // A 2x2 mip still occupies one whole block.
        CHECK(RegionBytes(TextureFormat::BC2, 2, 2) == 16);
        CHECK(RegionBytes(TextureFormat::BC2, 864, 1024) == 864 * 1024);
    }

    TEST_CASE("RegionBytes uses bytes per pixel for uncompressed formats") {
        CHECK(RegionBytes(TextureFormat::RGBA8, 3, 5) == 3 * 5 * 4);
        CHECK(RegionBytes(TextureFormat::RG8, 3, 5) == 3 * 5 * 2);
        CHECK(RegionBytes(TextureFormat::R8, 3, 5) == 3 * 5);
        CHECK(RegionBytes(TextureFormat::RGBA16F, 2, 2) == 2 * 2 * 8);
    }

    TEST_CASE("RegionBytes rejects non-positive and implausible sizes") {
        CHECK(RegionBytes(TextureFormat::RGBA8, 0, 4) == -1);
        CHECK(RegionBytes(TextureFormat::RGBA8, 4, -1) == -1);
        CHECK(RegionBytes(TextureFormat::RGBA8, 1 << 20, 4) == -1);
    }

    TEST_CASE("ParseCondition reads the property name and the value it must equal") {
        auto c = ParseCondition(2, R"({"condition":{"condition":"Isa","name":"facestyles"}})");
        REQUIRE(c.has_value());
        CHECK(c->index == 2);
        CHECK(c->property == "facestyles");
        CHECK(c->value == "Isa");
    }

    TEST_CASE("ParseCondition rejects malformed records") {
        CHECK_FALSE(ParseCondition(1, "not json").has_value());
        CHECK_FALSE(ParseCondition(1, R"({"condition":"Isa"})").has_value());
        CHECK_FALSE(ParseCondition(1, R"({"condition":{"condition":"Isa"}})").has_value());
        CHECK_FALSE(ParseCondition(1, R"({"condition":{"name":"facestyles"}})").has_value());
        // Index 0 is the base image; a condition can never select it.
        CHECK_FALSE(ParseCondition(0, R"({"condition":{"condition":"Isa","name":"facestyles"}})")
                        .has_value());
    }

    TEST_CASE("SelectVariant returns the first matching condition, else the base image") {
        std::vector<TexVariantCondition> conds { { 1, "face", "Ariane" },
                                                 { 2, "face", "Isa" },
                                                 { 3, "hair", "Isa" } };
        auto                             pick = [](std::string face, std::string hair) {
            return [face, hair](const std::string& prop, const std::string& value) {
                return (prop == "face" ? face : hair) == value;
            };
        };
        CHECK(SelectVariant(conds, pick("Elster", "Elster")) == 0);
        CHECK(SelectVariant(conds, pick("Isa", "Elster")) == 2);
        CHECK(SelectVariant(conds, pick("Elster", "Isa")) == 3);
        CHECK(SelectVariant(conds, pick("Ariane", "Isa")) == 1);
        CHECK(SelectVariant(conds, nullptr) == 0);
    }

    TEST_CASE("BlitPatch replaces whole block rows inside a BC2 mip") {
        // 12x8 mip = 3x2 blocks of 16 bytes; every block filled with its index.
        std::vector<uint8_t> mip(6 * 16);
        for (size_t b = 0; b < 6; b++) std::fill_n(mip.begin() + (long)(b * 16), 16, (uint8_t)b);
        // 8x4 patch at (4,4) covers blocks (1,1) and (2,1) = indices 4 and 5.
        std::vector<uint8_t> patch(2 * 16, 0xEE);
        REQUIRE(BlitPatch(TextureFormat::BC2,
                          mip.data(),
                          (isize)mip.size(),
                          12,
                          8,
                          patch.data(),
                          (isize)patch.size(),
                          4,
                          4,
                          8,
                          4));
        for (size_t b = 0; b < 4; b++) CHECK(mip[b * 16] == b);
        CHECK(mip[4 * 16] == 0xEE);
        CHECK(mip[5 * 16 + 15] == 0xEE);
    }

    TEST_CASE("BlitPatch copies pixel rows for RGBA8") {
        std::vector<uint8_t> mip(4 * 3 * 4, 0);
        std::vector<uint8_t> patch(2 * 2 * 4);
        std::iota(patch.begin(), patch.end(), (uint8_t)1);
        REQUIRE(BlitPatch(TextureFormat::RGBA8,
                          mip.data(),
                          (isize)mip.size(),
                          4,
                          3,
                          patch.data(),
                          (isize)patch.size(),
                          1,
                          1,
                          2,
                          2));
        // Row 1 starts at byte 16; pixel 1 at +4.
        CHECK(mip[16 + 4] == 1);
        CHECK(mip[16 + 11] == 8);
        CHECK(mip[32 + 4] == 9);
        CHECK(mip[16 + 0] == 0);
        CHECK(mip[16 + 12] == 0);
    }

    TEST_CASE("BlitPatch rejects patches that do not fit the mip") {
        std::vector<uint8_t> mip(4 * 16, 0);
        std::vector<uint8_t> patch(16, 0xEE);
        auto                 blit = [&](i32 x, i32 y, i32 w, i32 h, isize psize) {
            return BlitPatch(TextureFormat::BC2,
                             mip.data(),
                             (isize)mip.size(),
                             8,
                             8,
                             patch.data(),
                             psize,
                             x,
                             y,
                             w,
                             h);
        };
        CHECK_FALSE(blit(2, 0, 4, 4, 16));  // not block-aligned
        CHECK_FALSE(blit(8, 0, 4, 4, 16));  // past the right edge
        CHECK_FALSE(blit(4, 4, 8, 4, 16));  // overruns the width
        CHECK_FALSE(blit(0, -4, 4, 4, 16)); // negative offset
        CHECK_FALSE(blit(0, 0, 4, 4, 8));   // patch shorter than its rectangle
        CHECK(std::all_of(mip.begin(), mip.end(), [](uint8_t v) {
            return v == 0;
        }));
        CHECK(blit(4, 4, 4, 4, 16));
    }

    TEST_CASE("BlitPatch rejects a mip buffer of the wrong size") {
        std::vector<uint8_t> mip(3 * 16, 0);
        std::vector<uint8_t> patch(16, 0xEE);
        CHECK_FALSE(BlitPatch(TextureFormat::BC2,
                              mip.data(),
                              (isize)mip.size(),
                              8,
                              8,
                              patch.data(),
                              (isize)patch.size(),
                              0,
                              0,
                              4,
                              4));
    }
}
