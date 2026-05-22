#include <doctest.h>
#include "Scene/ShaderCacheKey.h"
#include "Core/MapSet.hpp"
#include "Type.hpp"

#include <array>
#include <string>
#include <vector>

using namespace wallpaper;

using Combos = Map<std::string, std::string>; // mirrors WPShaderParser.hpp's alias

// The SPV disk cache is keyed on the inputs that fully determine the compiled
// shader -- each unit's raw source + stage, plus the combos dict -- so a warm
// cache can be consulted WITHOUT running the (expensive) Preprocessor +
// glslang-preprocess pass.  These cases lock the discriminator's contract:
// identical inputs hash equal; any change to a source, a stage, a combo, the
// unit set, or the version constant changes the hash; combos order does not.
namespace
{
using Unit = ShaderCacheUnitInput;
Combos combosOf(std::initializer_list<std::pair<const std::string, std::string>> kv) {
    Combos c;
    for (auto& [k, v] : kv) c[k] = v;
    return c;
}
} // namespace

TEST_SUITE("ShaderCacheKey") {
    TEST_CASE("identical raw units + combos hash equal (deterministic)") {
        std::vector<Unit> u { { ShaderType::VERTEX, "void main(){}" },
                              { ShaderType::FRAGMENT, "void main(){}" } };
        auto              combos = combosOf({ { "FOO", "1" } });
        CHECK(shaderCacheKey(u, combos) == shaderCacheKey(u, combos));
    }
    TEST_CASE("a one-char change in a unit source changes the key") {
        std::vector<Unit> a { { ShaderType::FRAGMENT, "void main(){ }" } };
        std::vector<Unit> b { { ShaderType::FRAGMENT, "void main(){  }" } };
        Combos            none;
        CHECK(shaderCacheKey(a, none) != shaderCacheKey(b, none));
    }
    TEST_CASE("same source text under a different stage changes the key") {
        std::vector<Unit> v { { ShaderType::VERTEX, "void main(){}" } };
        std::vector<Unit> f { { ShaderType::FRAGMENT, "void main(){}" } };
        Combos            none;
        CHECK(shaderCacheKey(v, none) != shaderCacheKey(f, none));
    }
    TEST_CASE("flipping a combo value changes the key") {
        std::vector<Unit> u { { ShaderType::FRAGMENT, "void main(){}" } };
        CHECK(shaderCacheKey(u, combosOf({ { "BLOOM", "0" } })) !=
              shaderCacheKey(u, combosOf({ { "BLOOM", "1" } })));
    }
    TEST_CASE("adding a combo entry changes the key") {
        std::vector<Unit> u { { ShaderType::FRAGMENT, "void main(){}" } };
        CHECK(shaderCacheKey(u, combosOf({ { "A", "1" } })) !=
              shaderCacheKey(u, combosOf({ { "A", "1" }, { "B", "1" } })));
    }
    TEST_CASE("empty combos differs from one combo") {
        std::vector<Unit> u { { ShaderType::FRAGMENT, "void main(){}" } };
        Combos            none;
        CHECK(shaderCacheKey(u, none) != shaderCacheKey(u, combosOf({ { "A", "1" } })));
    }
    TEST_CASE("combo insertion order does not affect the key (ordered map)") {
        std::vector<Unit> u { { ShaderType::FRAGMENT, "void main(){}" } };
        Combos            forward;
        forward["A"] = "1";
        forward["B"] = "2";
        Combos backward;
        backward["B"] = "2";
        backward["A"] = "1";
        CHECK(shaderCacheKey(u, forward) == shaderCacheKey(u, backward));
    }
    TEST_CASE("unit count matters: {ab} != {a,b} with the same concatenated text") {
        std::vector<Unit> one { { ShaderType::VERTEX, "ab" } };
        std::vector<Unit> two { { ShaderType::VERTEX, "a" }, { ShaderType::VERTEX, "b" } };
        Combos            none;
        CHECK(shaderCacheKey(one, none) != shaderCacheKey(two, none));
    }
    TEST_CASE("a separator char in a source cannot forge another unit boundary") {
        // Defend the delimiter choice: control chars used as separators must
        // not let crafted source text collide with a different unit layout.
        std::vector<Unit> one { { ShaderType::VERTEX,
                                  "a\x1e\x1f"
                                  "b" } };
        std::vector<Unit> two { { ShaderType::VERTEX, "a" }, { ShaderType::VERTEX, "b" } };
        Combos            none;
        CHECK(shaderCacheKey(one, none) != shaderCacheKey(two, none));
    }
    TEST_CASE("the version constant participates in the key") {
        // Same inputs hashed with the shipped version vs a bumped version must
        // differ, proving a cache-version bump cleanly invalidates old entries.
        std::vector<Unit> u { { ShaderType::FRAGMENT, "void main(){}" } };
        Combos            none;
        CHECK(shaderCacheKey(u, none) !=
              shaderCacheKeyWithVersion(u, none, kShaderCacheKeyVersion + 1));
    }
} // TEST_SUITE ShaderCacheKey
