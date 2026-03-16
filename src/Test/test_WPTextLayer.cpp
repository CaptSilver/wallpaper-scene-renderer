#include <doctest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <regex>

// ===========================================================================
// NBSP normalization — mirrors stripESModuleSyntax's first step
// ===========================================================================

namespace
{

// Pure C++ version of the NBSP→space normalization used in SceneBackend.cpp
std::string normalizeNBSP(const std::string& src) {
    std::string result;
    result.reserve(src.size());
    for (size_t i = 0; i < src.size(); i++) {
        // UTF-8 for U+00A0 (NBSP) is 0xC2 0xA0
        if (i + 1 < src.size() && static_cast<unsigned char>(src[i]) == 0xC2 &&
            static_cast<unsigned char>(src[i + 1]) == 0xA0) {
            result += ' ';
            i++; // skip second byte
        } else {
            result += src[i];
        }
    }
    return result;
}

// Pure C++ version of the export stripping logic (post-NBSP normalization)
std::string stripExports(const std::string& src) {
    std::string result = src;
    // export function → function
    result = std::regex_replace(result, std::regex(R"(\bexport\s+function\b)"), "function");
    // export var → var
    result = std::regex_replace(result, std::regex(R"(\bexport\s+var\b)"), "var");
    // export let → let
    result = std::regex_replace(result, std::regex(R"(\bexport\s+let\b)"), "let");
    // export const → const
    result = std::regex_replace(result, std::regex(R"(\bexport\s+const\b)"), "const");
    // export class → class
    result = std::regex_replace(result, std::regex(R"(\bexport\s+class\b)"), "class");
    // export default (followed by content)
    result = std::regex_replace(result, std::regex(R"(\bexport\s+default\s+)"), "");
    return result;
}

} // anonymous namespace

// ===========================================================================
// NBSP normalization
// ===========================================================================

TEST_SUITE("NBSP normalization") {
    TEST_CASE("converts NBSP between export and var") {
        // "export\u00A0var" in UTF-8
        std::string in  = "export\xC2\xA0var x = 1;";
        std::string out = normalizeNBSP(in);
        CHECK(out == "export var x = 1;");
    }

    TEST_CASE("converts multiple NBSP in one line") {
        // "export\u00A0function\u00A0update\u00A0(value)"
        std::string in  = "export\xC2\xA0"
                          "function\xC2\xA0"
                          "update\xC2\xA0(value)";
        std::string out = normalizeNBSP(in);
        CHECK(out == "export function update (value)");
    }

    TEST_CASE("preserves regular spaces") {
        std::string in = "export function update(value)";
        CHECK(normalizeNBSP(in) == in);
    }

    TEST_CASE("preserves other UTF-8 sequences") {
        // Chinese characters are multi-byte but not 0xC2 0xA0
        std::string in = "// \xe4\xb8\xad\xe6\x96\x87 comment";
        CHECK(normalizeNBSP(in) == in);
    }

    TEST_CASE("empty string unchanged") { CHECK(normalizeNBSP("") == ""); }

    TEST_CASE("NBSP at start and end of string") {
        std::string in = "\xC2\xA0"
                         "hello\xC2\xA0";
        CHECK(normalizeNBSP(in) == " hello ");
    }

} // TEST_SUITE

// ===========================================================================
// Export stripping (post-NBSP normalization)
// ===========================================================================

TEST_SUITE("Export stripping") {
    TEST_CASE("strips export function") {
        CHECK(stripExports("export function update(v) {}") == "function update(v) {}");
    }

    TEST_CASE("strips export var") { CHECK(stripExports("export var x = 1;") == "var x = 1;"); }

    TEST_CASE("strips export let") {
        CHECK(stripExports("export let __id = '123';") == "let __id = '123';");
    }

    TEST_CASE("strips export const") {
        CHECK(stripExports("export const PI = 3.14;") == "const PI = 3.14;");
    }

    TEST_CASE("strips export class") {
        CHECK(stripExports("export class Foo {}") == "class Foo {}");
    }

    TEST_CASE("strips export default") {
        CHECK(stripExports("export default function foo() {}") == "function foo() {}");
    }

    TEST_CASE("strips multiple exports in multiline") {
        std::string in  = "export let id = '123';\n"
                          "export var props = {};\n"
                          "export function update(v) { return v; }\n";
        std::string out = stripExports(in);
        CHECK(out.find("export") == std::string::npos);
        CHECK(out.find("let id") != std::string::npos);
        CHECK(out.find("var props") != std::string::npos);
        CHECK(out.find("function update") != std::string::npos);
    }

    TEST_CASE("preserves exports inside strings") {
        // 'export' inside a string literal — regex won't have word boundary issues
        // since it's preceded by a quote, not a word char
        std::string in = "var s = 'do not export this';";
        CHECK(stripExports(in) == in);
    }

    TEST_CASE("NBSP + export stripping combined") {
        std::string in         = "export\xC2\xA0"
                                 "function\xC2\xA0"
                                 "update\xC2\xA0(v) {}";
        std::string normalized = normalizeNBSP(in);
        std::string stripped   = stripExports(normalized);
        CHECK(stripped == "function update (v) {}");
    }

    TEST_CASE("no exports leaves source unchanged") {
        std::string in = "function foo() { return 42; }";
        CHECK(stripExports(in) == in);
    }

} // TEST_SUITE

// ===========================================================================
// Text layer: pointsize user property binding detection
// ===========================================================================

TEST_SUITE("Text pointsize user property binding") {
    TEST_CASE("detects simple user property binding") {
        nlohmann::json json = { { "id", 369 },
                                { "pointsize", { { "user", "newproperty8" }, { "value", 16 } } } };
        // Simulate WPTextObject::FromJson binding detection logic
        std::string pointsizeUserProp;
        if (json.contains("pointsize") && json.at("pointsize").is_object() &&
            json.at("pointsize").contains("user")) {
            const auto& userField = json.at("pointsize")["user"];
            if (userField.is_string()) pointsizeUserProp = userField.get<std::string>();
        }
        CHECK(pointsizeUserProp == "newproperty8");
    }

    TEST_CASE("scalar pointsize has no binding") {
        nlohmann::json json = { { "id", 369 }, { "pointsize", 16.0 } };
        std::string    pointsizeUserProp;
        if (json.contains("pointsize") && json.at("pointsize").is_object() &&
            json.at("pointsize").contains("user")) {
            const auto& userField = json.at("pointsize")["user"];
            if (userField.is_string()) pointsizeUserProp = userField.get<std::string>();
        }
        CHECK(pointsizeUserProp.empty());
    }

    TEST_CASE("missing pointsize has no binding") {
        nlohmann::json json = { { "id", 369 } };
        std::string    pointsizeUserProp;
        if (json.contains("pointsize") && json.at("pointsize").is_object() &&
            json.at("pointsize").contains("user")) {
            const auto& userField = json.at("pointsize")["user"];
            if (userField.is_string()) pointsizeUserProp = userField.get<std::string>();
        }
        CHECK(pointsizeUserProp.empty());
    }

    TEST_CASE("object pointsize without user field has no binding") {
        nlohmann::json json = { { "pointsize", { { "value", 32 } } } };
        std::string    pointsizeUserProp;
        if (json.contains("pointsize") && json.at("pointsize").is_object() &&
            json.at("pointsize").contains("user")) {
            const auto& userField = json.at("pointsize")["user"];
            if (userField.is_string()) pointsizeUserProp = userField.get<std::string>();
        }
        CHECK(pointsizeUserProp.empty());
    }

    TEST_CASE("non-string user field ignored") {
        nlohmann::json json = { { "pointsize", { { "user", 123 }, { "value", 16 } } } };
        std::string    pointsizeUserProp;
        if (json.contains("pointsize") && json.at("pointsize").is_object() &&
            json.at("pointsize").contains("user")) {
            const auto& userField = json.at("pointsize")["user"];
            if (userField.is_string()) pointsizeUserProp = userField.get<std::string>();
        }
        CHECK(pointsizeUserProp.empty());
    }

} // TEST_SUITE

// ===========================================================================
// Text layer: scriptProperties extraction from text sub-object
// ===========================================================================

TEST_SUITE("Text scriptProperties extraction") {
    TEST_CASE("extracts scriptproperties from text sub-object") {
        nlohmann::json scriptProps = { { "use24hFormat", { { "value", true } } },
                                       { "displayDate", { { "value", false } } } };
        nlohmann::json json        = { { "text",
                                         { { "value", "12:34:56" },
                                           { "script", "export function update(v) { return v; }" },
                                           { "scriptproperties", scriptProps } } } };
        // Simulate WPTextObject extraction logic
        std::string textScriptProperties;
        if (json.contains("text") && json.at("text").is_object()) {
            const auto& jText = json.at("text");
            if (jText.contains("scriptproperties"))
                textScriptProperties = jText.at("scriptproperties").dump();
        }
        CHECK(! textScriptProperties.empty());
        // Verify round-trip: parse back and check keys
        auto parsed = nlohmann::json::parse(textScriptProperties);
        CHECK(parsed.contains("use24hFormat"));
        CHECK(parsed.contains("displayDate"));
    }

    TEST_CASE("text as plain string has no scriptProperties") {
        nlohmann::json json = { { "text", "Hello World" } };
        std::string    textScriptProperties;
        if (json.contains("text") && json.at("text").is_object()) {
            const auto& jText = json.at("text");
            if (jText.contains("scriptproperties"))
                textScriptProperties = jText.at("scriptproperties").dump();
        }
        CHECK(textScriptProperties.empty());
    }

    TEST_CASE("text object without scriptproperties") {
        nlohmann::json json = {
            { "text", { { "value", "12:00" }, { "script", "function update(v) {}" } } }
        };
        std::string textScriptProperties;
        if (json.contains("text") && json.at("text").is_object()) {
            const auto& jText = json.at("text");
            if (jText.contains("scriptproperties"))
                textScriptProperties = jText.at("scriptproperties").dump();
        }
        CHECK(textScriptProperties.empty());
    }

    TEST_CASE("missing text field") {
        nlohmann::json json = { { "id", 100 } };
        std::string    textScriptProperties;
        if (json.contains("text") && json.at("text").is_object()) {
            const auto& jText = json.at("text");
            if (jText.contains("scriptproperties"))
                textScriptProperties = jText.at("scriptproperties").dump();
        }
        CHECK(textScriptProperties.empty());
    }

} // TEST_SUITE
