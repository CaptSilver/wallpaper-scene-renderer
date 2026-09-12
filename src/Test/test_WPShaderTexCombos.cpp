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

// The spin effect declares its mask sampler inside the very conditional the
// mask combo controls.  Its default must not be read as "the scene bound a
// texture here", or the combo switches itself on and the shader blends against
// a black mask — which for spin means the untransformed sample wins and the
// layer stops spinning.
const char* kSelfGuardedCombo =
    "#if MASK == 1\n"
    "uniform sampler2D g_Texture1; // "
    "{\"combo\":\"MASK\",\"default\":\"util/black\",\"mode\":\"opacitymask\"}\n"
    "#endif\n"
    "void main() {}\n";

// A different combo in the guard must not suppress this one, and neither may a
// name this one merely prefixes.
const char* kForeignGuardedCombo = "#if MASKED == 1\n"
                                   "uniform sampler2D g_Texture1; // "
                                   "{\"combo\":\"MASK\",\"default\":\"util/black\"}\n"
                                   "#endif\n"
                                   "void main() {}\n";

// Once the guard closes, a later declaration is unconditional again.
const char* kComboAfterGuardCloses = "#if MASK == 1\n"
                                     "float unused;\n"
                                     "#endif\n"
                                     "uniform sampler2D g_Texture1; // "
                                     "{\"combo\":\"MASK\",\"default\":\"util/white\"}\n"
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

    TEST_CASE("a texture declared inside its own combo guard does not enable that combo") {
        std::vector<WPShaderTexInfo> texs { { true }, { false } };
        auto                         combos = parse(kSelfGuardedCombo, texs);
        REQUIRE(combos.count("MASK") == 1);
        CHECK(combos.at("MASK") == "0");
    }

    TEST_CASE("a guard naming a different combo still lets the default count") {
        std::vector<WPShaderTexInfo> texs { { true }, { false } };
        auto                         combos = parse(kForeignGuardedCombo, texs);
        REQUIRE(combos.count("MASK") == 1);
        CHECK(combos.at("MASK") == "1");
    }

    TEST_CASE("a declaration after the guard closes is unconditional again") {
        std::vector<WPShaderTexInfo> texs { { true }, { false } };
        auto                         combos = parse(kComboAfterGuardCloses, texs);
        REQUIRE(combos.count("MASK") == 1);
        CHECK(combos.at("MASK") == "1");
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
