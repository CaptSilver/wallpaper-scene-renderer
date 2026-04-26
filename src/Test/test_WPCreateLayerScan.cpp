#include <doctest.h>

#include <regex>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Regex pattern coverage for the SceneScript pre-scan additions in commit
// 09c3470.  WPSceneParser.cpp keeps the regexes as `static const std::regex`
// inside a lambda; we mirror them here verbatim so a future change to the
// pattern string has to update both copies, breaking the test if the new
// pattern doesn't match the sample scripts that motivated the original.
//
// Patterns under test:
//   1. createLayer string form: `createLayer('models/bar.json')`
//   2. __workshopId declaration: `export let __workshopId = 'NNN';`
//   3. for-loop bound: `for (var i = 1; i < 64; ++i) { ... }`
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

const std::regex createLayerStringRe(
    R"(createLayer\s*\(\s*['"]([^'"]+)['"]\s*\))");

const std::regex workshopIdRe(R"(__workshopId\s*=\s*['"]([^'"]+)['"])");

const std::regex forBoundRe(R"(for\s*\([^)]*?\b\w+\s*<=?\s*(\d+))");

std::vector<std::string> matches(const std::regex& re, const std::string& s) {
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), re);
         it != std::sregex_iterator();
         ++it) {
        out.push_back((*it)[1].str());
    }
    return out;
}

} // namespace

TEST_SUITE("WPSceneParser createLayer pre-scan regex")
{
    TEST_CASE("createLayer string — single quotes") {
        auto m = matches(createLayerStringRe,
                         "let bar = thisScene.createLayer('models/bar.json');");
        CHECK(m == std::vector<std::string> { "models/bar.json" });
    }

    TEST_CASE("createLayer string — double quotes") {
        auto m = matches(createLayerStringRe,
                         R"(thisScene.createLayer("models/foo.json"))");
        CHECK(m == std::vector<std::string> { "models/foo.json" });
    }

    TEST_CASE("createLayer string — multiple in same script") {
        auto m = matches(
            createLayerStringRe,
            "var a = thisScene.createLayer('models/bar.json');\n"
            "var b = thisScene.createLayer('models/star.json');\n");
        REQUIRE(m.size() == 2);
        CHECK(m[0] == "models/bar.json");
        CHECK(m[1] == "models/star.json");
    }

    TEST_CASE("createLayer object literal — does NOT match string regex") {
        auto m = matches(createLayerStringRe,
                         "thisScene.createLayer({image:'models/x.json'})");
        CHECK(m.empty());
    }

    TEST_CASE("createLayer with no arg — does NOT match") {
        auto m = matches(createLayerStringRe, "thisScene.createLayer()");
        CHECK(m.empty());
    }
}

TEST_SUITE("WPSceneParser __workshopId regex")
{
    TEST_CASE("export let form (Naruto Shippuden's bar script)") {
        auto m = matches(workshopIdRe,
                         "export let __workshopId = '2092495494';");
        CHECK(m == std::vector<std::string> { "2092495494" });
    }

    TEST_CASE("var form, double quotes") {
        auto m = matches(workshopIdRe, R"(var __workshopId = "1234567";)");
        CHECK(m == std::vector<std::string> { "1234567" });
    }

    TEST_CASE("missing — empty match") {
        auto m = matches(workshopIdRe,
                         "// no workshop id in this script");
        CHECK(m.empty());
    }

    TEST_CASE("first match wins (regex_search style)") {
        std::smatch sm;
        std::string s = "let __workshopId = '111';\n"
                         "let __workshopId = '222';\n";
        REQUIRE(std::regex_search(s, sm, workshopIdRe));
        CHECK(sm[1].str() == "111");
    }
}

TEST_SUITE("WPSceneParser for-loop bound regex")
{
    TEST_CASE("standard form: for (var i = 0; i < 64; ++i)") {
        auto m = matches(forBoundRe, "for (var i = 0; i < 64; ++i) {}");
        CHECK(m == std::vector<std::string> { "64" });
    }

    TEST_CASE("from-1 form: for (var i = 1; i < 64; ++i) — used by Naruto "
              "Shippuden's audio bars") {
        auto m = matches(forBoundRe, "for (var i = 1; i < 64; ++i) {}");
        CHECK(m == std::vector<std::string> { "64" });
    }

    TEST_CASE("less-than-or-equal form: for (var i = 0; i <= 31; ...)") {
        auto m = matches(forBoundRe, "for (var i = 0; i <= 31; ++i) {}");
        CHECK(m == std::vector<std::string> { "31" });
    }

    TEST_CASE("multiple loops — picks all bounds") {
        auto m = matches(forBoundRe,
                         "for (var i = 0; i < 8; ++i) {}\n"
                         "for (var j = 0; j < 64; ++j) {}\n");
        REQUIRE(m.size() == 2);
        CHECK(m[0] == "8");
        CHECK(m[1] == "64");
    }

    TEST_CASE("non-numeric bound — does NOT match (we only seed pool sizes "
              "from integer literals)") {
        auto m = matches(forBoundRe,
                         "for (var i = 0; i < someVar; ++i) {}");
        CHECK(m.empty());
    }
}
