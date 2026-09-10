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

        CHECK(m.userShaderBindings.at("earthcolor") == "color");
        CHECK(m.userShaderBindings.at("cloudopacity") == "alpha");
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

        CHECK(m.userShaderBindings.at("nightcolor") == "color");
    }
}
