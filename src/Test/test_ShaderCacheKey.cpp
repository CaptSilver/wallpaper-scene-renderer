#include <doctest.h>

#include <string>
#include <vector>

#include "WPShaderParser.hpp"

using wallpaper::ShaderCacheKey;
using wallpaper::ShaderTranslatorFingerprint;
using wallpaper::WPShaderUnit;

namespace
{
std::vector<WPShaderUnit> makeUnits(const std::string& vert, const std::string& frag) {
    std::vector<WPShaderUnit> units(2);
    units[0].src = vert;
    units[1].src = frag;
    return units;
}
} // namespace

TEST_SUITE("shader SPV cache key") {
    TEST_CASE("same sources and same salt give the same key") {
        auto a = makeUnits("void main(){}", "void main(){ }");
        auto b = makeUnits("void main(){}", "void main(){ }");
        CHECK(ShaderCacheKey(a, "salt") == ShaderCacheKey(b, "salt"));
    }

    TEST_CASE("a different salt changes the key for identical sources") {
        // The key is hashed before translation, so the translator's identity has
        // to come in through the salt or a translator fix never invalidates a
        // warm cache.
        auto units = makeUnits("void main(){}", "void main(){ }");
        CHECK(ShaderCacheKey(units, "translator-A") != ShaderCacheKey(units, "translator-B"));
    }

    TEST_CASE("different sources give different keys under the same salt") {
        auto a = makeUnits("void main(){}", "void main(){ }");
        auto b = makeUnits("void main(){}", "void main(){ x; }");
        CHECK(ShaderCacheKey(a, "salt") != ShaderCacheKey(b, "salt"));
    }

    TEST_CASE("the compiled-in translator fingerprint is a SHA-1 hex string") {
        auto fp = ShaderTranslatorFingerprint();
        CHECK(fp.size() == 40);
        CHECK(fp.find_first_not_of("0123456789abcdef") == std::string_view::npos);
    }

    TEST_CASE("the default salt is the compiled-in fingerprint") {
        auto units = makeUnits("a", "b");
        CHECK(ShaderCacheKey(units) == ShaderCacheKey(units, ShaderTranslatorFingerprint()));
    }
}
