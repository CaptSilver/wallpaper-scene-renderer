#include <doctest.h>

#include "wpscene/WPMaterial.h"
#include "WPUserProperties.hpp"

#include <nlohmann/json.hpp>

using namespace wallpaper::wpscene;
using wallpaper::WPUserProperties;
using wallpaper::UserPropertiesScope;

namespace
{

WPMaterial Parse(const char* text) {
    auto       j = nlohmann::json::parse(text);
    WPMaterial m;
    REQUIRE(m.FromJson(j));
    return m;
}

bool TryParse(const char* text, WPMaterial& out) {
    auto j = nlohmann::json::parse(text);
    return out.FromJson(j);
}

} // namespace

TEST_SUITE("WPMaterialPassBindItem::FromJson") {
    TEST_CASE("name + index are captured") {
        auto j = nlohmann::json::parse(R"({"name": "tex0", "index": 3})");
        WPMaterialPassBindItem b;
        REQUIRE(b.FromJson(j));
        CHECK(b.name == "tex0");
        CHECK(b.index == 3);
    }
}

TEST_SUITE("WPMaterialPass::FromJson") {
    TEST_CASE("textures list parses; null entries become empty strings") {
        auto j = nlohmann::json::parse(R"({
            "textures": ["a.tex", null, "c.tex"]
        })");
        WPMaterialPass p;
        REQUIRE(p.FromJson(j));
        REQUIRE(p.textures.size() == 3u);
        CHECK(p.textures[0] == "a.tex");
        CHECK(p.textures[1] == "");
        CHECK(p.textures[2] == "c.tex");
    }

    TEST_CASE("constantshadervalues map parses (string vector form)") {
        auto j = nlohmann::json::parse(R"({
            "constantshadervalues": {"g_Color": "1.0 0.5 0.25 1.0"}
        })");
        WPMaterialPass p;
        REQUIRE(p.FromJson(j));
        REQUIRE(p.constantshadervalues.count("g_Color") == 1u);
        const auto& v = p.constantshadervalues.at("g_Color");
        REQUIRE(v.size() == 4u);
        CHECK(v[0] == doctest::Approx(1.0f));
        CHECK(v[3] == doctest::Approx(1.0f));
    }

    TEST_CASE("constantshadervalues scalar number broadcasts to single-element vector") {
        auto j = nlohmann::json::parse(R"({
            "constantshadervalues": {"g_Alpha": 0.5}
        })");
        WPMaterialPass p;
        REQUIRE(p.FromJson(j));
        REQUIRE(p.constantshadervalues.count("g_Alpha") == 1u);
        const auto& v = p.constantshadervalues.at("g_Alpha");
        REQUIRE(v.size() == 1u);
        CHECK(v[0] == doctest::Approx(0.5f));
    }

    TEST_CASE("combos map parses") {
        auto j = nlohmann::json::parse(R"({
            "combos": {"BLOOM": 1, "HDR": 0}
        })");
        WPMaterialPass p;
        REQUIRE(p.FromJson(j));
        CHECK(p.combos.at("BLOOM") == 1);
        CHECK(p.combos.at("HDR") == 0);
    }

    TEST_CASE("target field parses") {
        auto j = nlohmann::json::parse(R"({"target": "_rt_default"})");
        WPMaterialPass p;
        REQUIRE(p.FromJson(j));
        CHECK(p.target == "_rt_default");
    }

    TEST_CASE("bind list parses recursively") {
        auto j = nlohmann::json::parse(R"({
            "bind": [{"name": "src", "index": 0}, {"name": "ref", "index": 1}]
        })");
        WPMaterialPass p;
        REQUIRE(p.FromJson(j));
        REQUIRE(p.bind.size() == 2u);
        CHECK(p.bind[0].name == "src");
        CHECK(p.bind[1].index == 1);
    }
}

TEST_SUITE("WPMaterialPass::Update") {
    TEST_CASE("Update overlays textures + combos + constantshadervalues") {
        WPMaterialPass base;
        base.textures = { "a", "b" };
        base.combos["X"] = 0;
        base.constantshadervalues["g_C"] = {1.0f};

        WPMaterialPass overlay;
        overlay.textures = { "", "newB", "newC" }; // empty → don't overwrite slot 0
        overlay.combos["X"] = 1;
        overlay.combos["Y"] = 2;
        overlay.constantshadervalues["g_D"] = {2.0f};

        base.Update(overlay);

        REQUIRE(base.textures.size() == 3u);
        CHECK(base.textures[0] == "a");        // empty did not overwrite
        CHECK(base.textures[1] == "newB");
        CHECK(base.textures[2] == "newC");
        CHECK(base.combos.at("X") == 1);
        CHECK(base.combos.at("Y") == 2);
        CHECK(base.constantshadervalues.at("g_D")[0] == doctest::Approx(2.0f));
        CHECK(base.constantshadervalues.at("g_C")[0] == doctest::Approx(1.0f));
    }
}

TEST_SUITE("WPMaterial::FromJson — error paths") {
    TEST_CASE("missing passes is rejected") {
        WPMaterial m;
        CHECK_FALSE(TryParse(R"({})", m));
    }

    TEST_CASE("empty passes array is rejected") {
        WPMaterial m;
        CHECK_FALSE(TryParse(R"({"passes": []})", m));
    }

    TEST_CASE("first pass without shader is rejected") {
        WPMaterial m;
        CHECK_FALSE(TryParse(R"({"passes": [{"blending": "translucent"}]})", m));
    }
}

TEST_SUITE("WPMaterial::FromJson — happy paths") {
    TEST_CASE("minimal pass with shader parses") {
        auto m = Parse(R"({
            "passes": [{
                "shader":   "flat",
                "blending": "additive",
                "cullmode": "back",
                "depthtest":  "lequal",
                "depthwrite": "enabled"
            }]
        })");
        CHECK(m.shader == "flat");
        CHECK(m.blending == "additive");
        CHECK(m.cullmode == "back");
        CHECK(m.depthtest == "lequal");
        CHECK(m.depthwrite == "enabled");
    }

    TEST_CASE("textures with null slot parses as empty string") {
        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "textures": ["t0.tex", null, "t2.tex"]
            }]
        })");
        REQUIRE(m.textures.size() == 3u);
        CHECK(m.textures[0] == "t0.tex");
        CHECK(m.textures[1] == "");
        CHECK(m.textures[2] == "t2.tex");
    }

    TEST_CASE("constantshadervalues + combos populate") {
        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "constantshadervalues": {"g_Alpha": "1.0"},
                "combos": {"BLOOM": 1}
            }]
        })");
        CHECK(m.constantshadervalues.at("g_Alpha")[0] == doctest::Approx(1.0f));
        CHECK(m.combos.at("BLOOM") == 1);
    }

    TEST_CASE("alternate spelling depthtesting/depthwriting respected when canonical absent") {
        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "depthtesting":  "always",
                "depthwriting":  "enabled"
            }]
        })");
        CHECK(m.depthtest == "always");
        CHECK(m.depthwrite == "enabled");
    }

    TEST_CASE("canonical spelling wins when both present") {
        auto m = Parse(R"({
            "passes": [{
                "shader":       "flat",
                "depthtest":    "lequal",
                "depthtesting": "always"
            }]
        })");
        CHECK(m.depthtest == "lequal");
    }

    TEST_CASE("usershadervalues recorded in userShaderBindings without g_currentUserProperties") {
        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"opacity": "g_Alpha", "tint": "g_Color"}
            }]
        })");
        CHECK(m.userShaderBindings.at("opacity") == "g_Alpha");
        CHECK(m.userShaderBindings.at("tint")    == "g_Color");
    }
}

TEST_SUITE("WPMaterial::MergePass") {
    TEST_CASE("MergePass overlays textures + maps from a pass into the material") {
        auto m = Parse(R"({
            "passes": [{
                "shader":   "flat",
                "textures": ["base.tex"],
                "combos":   {"X": 0}
            }]
        })");
        WPMaterialPass extra;
        extra.textures = { "newBase.tex", "extra.tex" };
        extra.combos["X"] = 5;
        extra.combos["Y"] = 7;
        extra.constantshadervalues["g_New"] = {0.25f};

        m.MergePass(extra);
        REQUIRE(m.textures.size() == 2u);
        CHECK(m.textures[0] == "newBase.tex");
        CHECK(m.textures[1] == "extra.tex");
        CHECK(m.combos.at("X") == 5);
        CHECK(m.combos.at("Y") == 7);
        CHECK(m.constantshadervalues.at("g_New")[0] == doctest::Approx(0.25f));
    }
}

// =====================================================================
// usershadervalues + g_currentUserProperties hookup (lines 135-154 in WPMaterial.cpp)
// Material exposes "usershadervalues": {"<userPropName>": "<shaderConstName>"} and
// when a thread-local g_currentUserProperties is active, it should resolve the
// user property's value into constantshadervalues[shaderConstName] as a float vector.
// =====================================================================
TEST_SUITE("WPMaterial::FromJson — usershadervalues with g_currentUserProperties") {
    TEST_CASE("string property: whitespace-separated floats are parsed into a vector") {
        WPUserProperties props;
        // Color-style 4-float string (typical for g_Color)
        props.SetProperty("tint", nlohmann::json("0.25 0.5 0.75 1.0"));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"tint": "g_Color"}
            }]
        })");
        // Binding always recorded
        CHECK(m.userShaderBindings.at("tint") == "g_Color");
        // Value resolved into constantshadervalues by user property hookup
        REQUIRE(m.constantshadervalues.count("g_Color") == 1u);
        const auto& v = m.constantshadervalues.at("g_Color");
        REQUIRE(v.size() == 4u);
        CHECK(v[0] == doctest::Approx(0.25f));
        CHECK(v[1] == doctest::Approx(0.5f));
        CHECK(v[2] == doctest::Approx(0.75f));
        CHECK(v[3] == doctest::Approx(1.0f));
    }

    TEST_CASE("string property: single-token value parses to one-element vector") {
        WPUserProperties props;
        props.SetProperty("opacity", nlohmann::json("0.42"));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"opacity": "g_Alpha"}
            }]
        })");
        REQUIRE(m.constantshadervalues.count("g_Alpha") == 1u);
        const auto& v = m.constantshadervalues.at("g_Alpha");
        REQUIRE(v.size() == 1u);
        CHECK(v[0] == doctest::Approx(0.42f));
    }

    TEST_CASE("string property: empty string yields no constant entry (floatVec stays empty)") {
        WPUserProperties props;
        props.SetProperty("nothing", nlohmann::json(""));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"nothing": "g_Empty"}
            }]
        })");
        // Binding still recorded
        CHECK(m.userShaderBindings.at("nothing") == "g_Empty");
        // But no value parsed, so no constantshadervalues entry
        CHECK(m.constantshadervalues.count("g_Empty") == 0u);
    }

    TEST_CASE("string property: non-numeric tokens parse zero floats, no constant emitted") {
        WPUserProperties props;
        props.SetProperty("garbage", nlohmann::json("not a number"));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"garbage": "g_Junk"}
            }]
        })");
        CHECK(m.userShaderBindings.at("garbage") == "g_Junk");
        CHECK(m.constantshadervalues.count("g_Junk") == 0u);
    }

    TEST_CASE("number property: scalar resolves to single-element vector") {
        WPUserProperties props;
        props.SetProperty("brightness", nlohmann::json(1.75));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"brightness": "g_Bright"}
            }]
        })");
        REQUIRE(m.constantshadervalues.count("g_Bright") == 1u);
        const auto& v = m.constantshadervalues.at("g_Bright");
        REQUIRE(v.size() == 1u);
        CHECK(v[0] == doctest::Approx(1.75f));
    }

    TEST_CASE("number property: integer is also accepted (is_number is true for ints)") {
        WPUserProperties props;
        props.SetProperty("count", nlohmann::json(7));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"count": "g_Count"}
            }]
        })");
        REQUIRE(m.constantshadervalues.count("g_Count") == 1u);
        const auto& v = m.constantshadervalues.at("g_Count");
        REQUIRE(v.size() == 1u);
        CHECK(v[0] == doctest::Approx(7.0f));
    }

    TEST_CASE("array property: numeric elements are pushed into the vector") {
        WPUserProperties props;
        // Array of numbers — typical for vec3/vec4 properties
        props.SetProperty("color", nlohmann::json({0.1, 0.2, 0.3}));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"color": "g_TintRGB"}
            }]
        })");
        REQUIRE(m.constantshadervalues.count("g_TintRGB") == 1u);
        const auto& v = m.constantshadervalues.at("g_TintRGB");
        REQUIRE(v.size() == 3u);
        CHECK(v[0] == doctest::Approx(0.1f));
        CHECK(v[1] == doctest::Approx(0.2f));
        CHECK(v[2] == doctest::Approx(0.3f));
    }

    TEST_CASE("array property: non-numeric elements are silently skipped") {
        WPUserProperties props;
        // Mixed array — only numeric entries should land in floatVec
        props.SetProperty("mixed", nlohmann::json::parse(R"([1.0, "skip", 2.0, null, true, 3.0])"));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"mixed": "g_Mixed"}
            }]
        })");
        REQUIRE(m.constantshadervalues.count("g_Mixed") == 1u);
        const auto& v = m.constantshadervalues.at("g_Mixed");
        REQUIRE(v.size() == 3u);
        CHECK(v[0] == doctest::Approx(1.0f));
        CHECK(v[1] == doctest::Approx(2.0f));
        CHECK(v[2] == doctest::Approx(3.0f));
    }

    TEST_CASE("array property: empty array yields no constant entry") {
        WPUserProperties props;
        props.SetProperty("empty", nlohmann::json::array());
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"empty": "g_Nope"}
            }]
        })");
        CHECK(m.userShaderBindings.at("empty") == "g_Nope");
        CHECK(m.constantshadervalues.count("g_Nope") == 0u);
    }

    TEST_CASE("array property: all-non-numeric array yields no constant entry") {
        WPUserProperties props;
        props.SetProperty("strs", nlohmann::json::parse(R"(["a", "b", "c"])"));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"strs": "g_Strs"}
            }]
        })");
        CHECK(m.userShaderBindings.at("strs") == "g_Strs");
        CHECK(m.constantshadervalues.count("g_Strs") == 0u);
    }

    TEST_CASE("missing property: g_currentUserProperties active but key absent → continue") {
        WPUserProperties props;
        props.SetProperty("other", nlohmann::json(1.0));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"unknown": "g_Missing"}
            }]
        })");
        // Binding still recorded for the unknown key
        CHECK(m.userShaderBindings.at("unknown") == "g_Missing");
        // No value in props → propVal has no value → continue → no constant entry
        CHECK(m.constantshadervalues.count("g_Missing") == 0u);
    }

    TEST_CASE("unsupported value type (bool) yields no constant entry") {
        WPUserProperties props;
        // Bool is neither string nor number nor array — none of the three branches fire,
        // floatVec stays empty, no constantshadervalues entry written.
        props.SetProperty("flag", nlohmann::json(true));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"flag": "g_Flag"}
            }]
        })");
        CHECK(m.userShaderBindings.at("flag") == "g_Flag");
        CHECK(m.constantshadervalues.count("g_Flag") == 0u);
    }

    TEST_CASE("unsupported value type (null) yields no constant entry") {
        WPUserProperties props;
        props.SetProperty("nothing", nlohmann::json(nullptr));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {"nothing": "g_Null"}
            }]
        })");
        CHECK(m.userShaderBindings.at("nothing") == "g_Null");
        CHECK(m.constantshadervalues.count("g_Null") == 0u);
    }

    TEST_CASE("multiple usershadervalues entries each resolve independently") {
        WPUserProperties props;
        props.SetProperty("a", nlohmann::json("0.5"));
        props.SetProperty("b", nlohmann::json(2.0));
        props.SetProperty("c", nlohmann::json({0.1, 0.2}));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "usershadervalues": {
                    "a": "g_A",
                    "b": "g_B",
                    "c": "g_C"
                }
            }]
        })");
        REQUIRE(m.constantshadervalues.at("g_A").size() == 1u);
        CHECK(m.constantshadervalues.at("g_A")[0] == doctest::Approx(0.5f));
        REQUIRE(m.constantshadervalues.at("g_B").size() == 1u);
        CHECK(m.constantshadervalues.at("g_B")[0] == doctest::Approx(2.0f));
        REQUIRE(m.constantshadervalues.at("g_C").size() == 2u);
        CHECK(m.constantshadervalues.at("g_C")[0] == doctest::Approx(0.1f));
        CHECK(m.constantshadervalues.at("g_C")[1] == doctest::Approx(0.2f));
    }

    TEST_CASE("usershadervalues entry may overwrite an existing constantshadervalues entry") {
        // If both constantshadervalues AND usershadervalues mention the same shader name,
        // the usershadervalues hookup runs after, so the user-prop value wins when active.
        WPUserProperties props;
        props.SetProperty("override", nlohmann::json(9.99));
        UserPropertiesScope scope(&props);

        auto m = Parse(R"({
            "passes": [{
                "shader": "flat",
                "constantshadervalues": {"g_X": 1.0},
                "usershadervalues":     {"override": "g_X"}
            }]
        })");
        REQUIRE(m.constantshadervalues.at("g_X").size() == 1u);
        CHECK(m.constantshadervalues.at("g_X")[0] == doctest::Approx(9.99f));
    }
}
