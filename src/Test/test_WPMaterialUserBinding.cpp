#include <doctest.h>

#include "wpscene/WPMaterial.h"
#include "WPUserProperties.hpp"

#include <nlohmann/json.hpp>

using namespace wallpaper;
using namespace wallpaper::wpscene;

namespace
{

WPMaterial parse(const char* json) {
    WPMaterial m;
    REQUIRE(m.FromJson(nlohmann::json::parse(json)));
    return m;
}

} // namespace

// Shipped materials bind a user property to a shader constant two ways: the
// `usershadervalues` block, and inline in `constantshadervalues` as
// {"user": "<prop>", "value": <default>}.  Both have to end up in
// userShaderBindings or the property can only be applied by reparsing the scene.
TEST_SUITE("WPMaterial user property bindings") {
    TEST_CASE("inline constantshadervalues user reference is recorded as a binding") {
        auto m = parse(R"({
            "passes": [{
                "shader": "generic4",
                "constantshadervalues": {
                    "color": {"user": "earthcolor", "value": "0.5 0.5 0.5"},
                    "alpha": {"user": "cloudopacity", "value": 0.95},
                    "brightness": 0.82
                }
            }]
        })");

        CHECK(m.userShaderBindings.at("color") == "earthcolor");
        CHECK(m.userShaderBindings.at("alpha") == "cloudopacity");
        CHECK(m.userShaderBindings.count("brightness") == 0u);

        // The embedded default still lands in the material.
        CHECK(m.constantshadervalues.at("color")[0] == doctest::Approx(0.5f));
        CHECK(m.constantshadervalues.at("alpha")[0] == doctest::Approx(0.95f));
    }

    TEST_CASE("conditional user reference binds the named property") {
        auto m = parse(R"({
            "passes": [{
                "shader": "generic4",
                "constantshadervalues": {
                    "color": {"user": {"name": "nightcolor", "condition": "1"},
                              "value": "1 1 1"}
                }
            }]
        })");

        CHECK(m.userShaderBindings.at("color") == "nightcolor");
    }

    // The scene's per-effect passes[] override is parsed as a WPMaterialPass and
    // layered onto the material with MergePass; the link to the property has to
    // survive that, and an override of a constant has to drop what the base
    // bound it to.
    TEST_CASE("a pass override's user reference merges into the material's bindings") {
        auto           m = parse(R"({"passes": [{"shader": "generic4"}]})");
        WPMaterialPass p;
        REQUIRE(p.FromJson(nlohmann::json::parse(R"({
            "constantshadervalues": { "pulse": {"user": "myprop", "value": 1} }
        })")));
        CHECK(p.userShaderBindings.at("pulse") == "myprop");

        m.MergePass(p);
        CHECK(m.userShaderBindings.at("pulse") == "myprop");
    }

    TEST_CASE("one property driving two constants keeps both bindings") {
        auto m = parse(R"({
            "passes": [{
                "shader": "generic4",
                "constantshadervalues": {
                    "fill": {"user": "dock", "value": "1 1 1"},
                    "line": {"user": "dock", "value": "0 0 0"}
                }
            }]
        })");
        CHECK(m.userShaderBindings.at("fill") == "dock");
        CHECK(m.userShaderBindings.at("line") == "dock");
    }

    TEST_CASE("a plain override value drops the base binding for that constant") {
        auto           m = parse(R"({
            "passes": [{
                "shader": "generic4",
                "constantshadervalues": {
                    "pulse": {"user": "base", "value": 1},
                    "keep": {"user": "other", "value": 1}
                }
            }]
        })");
        WPMaterialPass p;
        REQUIRE(p.FromJson(nlohmann::json::parse(R"({"constantshadervalues": {"pulse": 0.5}})")));
        m.MergePass(p);

        CHECK(m.userShaderBindings.count("pulse") == 0u);
        CHECK(m.userShaderBindings.at("keep") == "other");
        CHECK(m.constantshadervalues.at("pulse")[0] == doctest::Approx(0.5f));
    }

    TEST_CASE("an override bound to another property replaces the base's property") {
        auto           m = parse(R"({
            "passes": [{
                "shader": "generic4",
                "constantshadervalues": {"pulse": {"user": "base", "value": 1}}
            }]
        })");
        WPMaterialPass p;
        REQUIRE(p.FromJson(nlohmann::json::parse(
            R"({"constantshadervalues": {"pulse": {"user": "mine", "value": 1}}})")));
        m.MergePass(p);

        CHECK(m.userShaderBindings.size() == 1u);
        CHECK(m.userShaderBindings.at("pulse") == "mine");
    }

    TEST_CASE("a pass layered onto a pass carries its bindings too") {
        WPMaterialPass base;
        REQUIRE(base.FromJson(nlohmann::json::parse(
            R"({"constantshadervalues": {"a": {"user": "pa", "value": 1}, "b": {"user": "pb", "value": 1}}})")));
        WPMaterialPass over;
        REQUIRE(over.FromJson(nlohmann::json::parse(R"({"constantshadervalues": {"a": 2}})")));
        base.Update(over);

        CHECK(base.userShaderBindings.count("a") == 0u);
        CHECK(base.userShaderBindings.at("b") == "pb");
    }

    // The live path writes the property's raw value and cannot evaluate a
    // condition, so a conditional reference in a pass override stays out of the
    // runtime bindings (the scene reloads instead); the material file's own
    // pass keeps recording it.
    TEST_CASE("a conditional user reference is bound in a material but not in a pass override") {
        constexpr const char* kConditional =
            R"({"constantshadervalues": {"pulse": {"user": {"name": "myprop", "condition": "1"}, "value": 1}}})";

        WPMaterialPass p;
        REQUIRE(p.FromJson(nlohmann::json::parse(kConditional)));
        CHECK(p.userShaderBindings.count("pulse") == 0u);

        auto m = parse(R"({"passes": [{"shader": "generic4", "constantshadervalues":
            {"pulse": {"user": {"name": "myprop", "condition": "1"}, "value": 1}}}]})");
        CHECK(m.userShaderBindings.at("pulse") == "myprop");
    }
}
