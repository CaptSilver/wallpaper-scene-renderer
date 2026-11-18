#include <doctest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

#include "Fs/CBinaryStream.h"
#include "WPShaderParser.hpp"
#include "test_scratch.hpp"

using wallpaper::Combos;
using wallpaper::LoadPreprocessedUnit;
using wallpaper::PreprocessCacheKey;
using wallpaper::SavePreprocessedUnit;
using wallpaper::ShaderType;
using wallpaper::WPShaderUnit;

namespace
{
WPShaderUnit makeUnit(ShaderType stage, const std::string& src) {
    WPShaderUnit unit;
    unit.stage = stage;
    unit.src   = src;
    return unit;
}

std::string scratchFile(const char* name) {
    return wallpaper::test::ScratchDir("preprocess_cache", ::getpid()) + "/" + name;
}
} // namespace

TEST_SUITE("preprocessed-source cache") {
    TEST_CASE("the key is stable for the same inputs") {
        auto   unit = makeUnit(ShaderType::FRAGMENT, "void main(){}");
        Combos combos { { "LIGHTING", "1" } };
        CHECK(PreprocessCacheKey(unit, combos, "salt") == PreprocessCacheKey(unit, combos, "salt"));
    }

    TEST_CASE("the key changes with the source, stage, combos and salt") {
        auto   frag = makeUnit(ShaderType::FRAGMENT, "void main(){}");
        Combos combos { { "LIGHTING", "1" } };
        auto   base = PreprocessCacheKey(frag, combos, "salt");

        CHECK(PreprocessCacheKey(
                  makeUnit(ShaderType::FRAGMENT, "void main(){ }"), combos, "salt") != base);
        CHECK(PreprocessCacheKey(makeUnit(ShaderType::VERTEX, "void main(){}"), combos, "salt") !=
              base);
        CHECK(PreprocessCacheKey(frag, Combos { { "LIGHTING", "0" } }, "salt") != base);
        CHECK(PreprocessCacheKey(frag, Combos {}, "salt") != base);
        CHECK(PreprocessCacheKey(frag, combos, "other salt") != base);
    }

    TEST_CASE("a saved unit loads back unchanged") {
        auto unit = makeUnit(ShaderType::FRAGMENT, "#version 450\nvoid main(){}\n");
        unit.preprocess_info.input            = { { "v_TexCoord", "in vec2 v_TexCoord;" } };
        unit.preprocess_info.output           = { { "o_Color", "out vec4 o_Color;" } };
        unit.preprocess_info.active_tex_slots = { 0, 2 };

        const auto path = scratchFile("roundtrip.pp");
        SavePreprocessedUnit(unit, *wallpaper::fs::CreateCBinaryStreamW(path));

        WPShaderUnit loaded = makeUnit(ShaderType::FRAGMENT, "unchanged");
        REQUIRE(LoadPreprocessedUnit(*wallpaper::fs::CreateCBinaryStream(path), loaded));
        CHECK(loaded.src == unit.src);
        CHECK(loaded.preprocess_info.input == unit.preprocess_info.input);
        CHECK(loaded.preprocess_info.output == unit.preprocess_info.output);
        CHECK(loaded.preprocess_info.active_tex_slots == unit.preprocess_info.active_tex_slots);
    }

    TEST_CASE("a truncated file is a miss and leaves the unit alone") {
        auto unit = makeUnit(ShaderType::FRAGMENT, "#version 450\nvoid main(){}\n");
        unit.preprocess_info.active_tex_slots = { 1 };

        const auto path = scratchFile("truncated.pp");
        SavePreprocessedUnit(unit, *wallpaper::fs::CreateCBinaryStreamW(path));
        std::filesystem::resize_file(path, std::filesystem::file_size(path) - 2);

        WPShaderUnit loaded = makeUnit(ShaderType::FRAGMENT, "unchanged");
        CHECK_FALSE(LoadPreprocessedUnit(*wallpaper::fs::CreateCBinaryStream(path), loaded));
        CHECK(loaded.src == "unchanged");
        CHECK(loaded.preprocess_info.active_tex_slots.empty());
    }
}
