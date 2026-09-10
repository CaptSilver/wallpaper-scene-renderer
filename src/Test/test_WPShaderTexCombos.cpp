#include <doctest.h>

#include <string>
#include <vector>

#include "WPShaderParser.hpp"
#include "Fs/VFS.h"

using wallpaper::WPShaderInfo;
using wallpaper::WPShaderParser;
using wallpaper::WPShaderTexInfo;
using wallpaper::fs::VFS;

namespace
{
// The parser only looks at uniform declaration lines, so a bare declaration
// list is a complete shader as far as these tests are concerned.
wallpaper::Combos parse(const std::string& src, const std::vector<WPShaderTexInfo>& texs) {
    VFS          vfs;
    WPShaderInfo info;
    (void)WPShaderParser::PreShaderSrc(vfs, src, &info, texs);
    return info.combos;
}

const char* kNormalCombo =
    "uniform sampler2D g_Texture1; // {\"material\":\"normal\",\"combo\":\"NORMALMAP\"}\n"
    "void main() {}\n";

const char* kNormalComboWithDefault =
    "uniform sampler2D g_Texture1; // "
    "{\"material\":\"normal\",\"combo\":\"NORMALMAP\",\"default\":\"models/normal\"}\n"
    "void main() {}\n";

const char* kSlotZeroCombo =
    "uniform sampler2D g_Texture0; // {\"material\":\"albedo\",\"combo\":\"ALBEDO\"}\n"
    "void main() {}\n";

const char* kFormatCombo =
    "uniform sampler2D g_Texture1; // "
    "{\"material\":\"normal\",\"format\":\"normalmap\",\"formatcombo\":true}\n"
    "void main() {}\n";
} // namespace

TEST_SUITE("shader texture combos") {
    TEST_CASE("texture combo stays off when the slot exists but holds no texture") {
        std::vector<WPShaderTexInfo> texs { { true }, { false } };
        auto                         combos = parse(kNormalCombo, texs);
        REQUIRE(combos.count("NORMALMAP") == 1);
        CHECK(combos.at("NORMALMAP") == "0");
    }

    TEST_CASE("texture combo turns on for an occupied slot") {
        std::vector<WPShaderTexInfo> texs { { true }, { true } };
        auto                         combos = parse(kNormalCombo, texs);
        REQUIRE(combos.count("NORMALMAP") == 1);
        CHECK(combos.at("NORMALMAP") == "1");
    }

    TEST_CASE("texture combo turns on for an empty slot the shader gives a default") {
        // Scene parsing fills the slot from the shader-declared default after
        // this runs, so the combo has to anticipate it.
        std::vector<WPShaderTexInfo> texs { { true }, { false } };
        auto                         combos = parse(kNormalComboWithDefault, texs);
        REQUIRE(combos.count("NORMALMAP") == 1);
        CHECK(combos.at("NORMALMAP") == "1");
    }

    // Slot 0 is a real slot.  A lower bound that excludes it would silently
    // drop the combo for every shader whose texture is the first one.
    TEST_CASE("texture combo turns on for an occupied slot zero") {
        std::vector<WPShaderTexInfo> texs { { true } };
        auto                         combos = parse(kSlotZeroCombo, texs);
        REQUIRE(combos.count("ALBEDO") == 1);
        CHECK(combos.at("ALBEDO") == "1");
    }

    TEST_CASE("texture combo stays off for a slot past the end of the array") {
        std::vector<WPShaderTexInfo> texs { { true } };
        auto                         combos = parse(kNormalCombo, texs);
        REQUIRE(combos.count("NORMALMAP") == 1);
        CHECK(combos.at("NORMALMAP") == "0");
    }

    TEST_CASE("format combo stays off when the slot exists but holds no texture") {
        std::vector<WPShaderTexInfo> texs { { true }, { false } };
        auto                         combos = parse(kFormatCombo, texs);
        REQUIRE(combos.count("NORMALMAP") == 1);
        CHECK(combos.at("NORMALMAP") == "0");
    }

    TEST_CASE("format combo turns on for an occupied slot") {
        std::vector<WPShaderTexInfo> texs { { true }, { true } };
        auto                         combos = parse(kFormatCombo, texs);
        REQUIRE(combos.count("NORMALMAP") == 1);
        CHECK(combos.at("NORMALMAP") == "1");
    }

    TEST_CASE("all four texture components can drive a combo") {
        const char* src =
            "uniform sampler2D g_Texture1; // {\"material\":\"generic\",\"components\":["
            "{\"label\":\"a\",\"combo\":\"COMPO_A\"},"
            "{\"label\":\"b\",\"combo\":\"COMPO_B\"},"
            "{\"label\":\"c\",\"combo\":\"COMPO_C\"},"
            "{\"label\":\"d\",\"combo\":\"COMPO_D\"}]}\n"
            "void main() {}\n";
        std::vector<WPShaderTexInfo> texs { { true }, { true, { true, true, true, true } } };
        auto                         combos = parse(src, texs);
        CHECK(combos.count("COMPO_A") == 1);
        CHECK(combos.count("COMPO_B") == 1);
        CHECK(combos.count("COMPO_C") == 1);
        CHECK(combos.count("COMPO_D") == 1);
    }
}
