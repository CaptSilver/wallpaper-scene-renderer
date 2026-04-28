#include <doctest.h>

#include "wpscene/WPMaterial.h"

#include <nlohmann/json.hpp>

using namespace wallpaper::wpscene;

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
