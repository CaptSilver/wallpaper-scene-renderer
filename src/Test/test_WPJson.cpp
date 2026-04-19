#include <doctest.h>

#include "WPJson.hpp"
#include "WPUserProperties.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <vector>

using namespace wallpaper;
using njson = nlohmann::json;

// ===========================================================================
// ParseJson — source-level JSON parsing with lenient (comment-allowed) mode
// ===========================================================================

TEST_SUITE("WPJson.ParseJson") {

TEST_CASE("valid JSON object parses") {
    njson out;
    REQUIRE(PARSE_JSON(R"({"a":1,"b":"text"})", out));
    CHECK(out["a"].get<int>() == 1);
    CHECK(out["b"].get<std::string>() == "text");
}

TEST_CASE("valid JSON array parses") {
    njson out;
    REQUIRE(PARSE_JSON("[1,2,3]", out));
    REQUIRE(out.is_array());
    CHECK(out.size() == 3);
}

TEST_CASE("JSON with comments is accepted (lenient mode)") {
    njson out;
    // nlohmann::json::parse is invoked with ignore_comments=true
    REQUIRE(PARSE_JSON(R"({"a":1 /* trailing comment */})", out));
    CHECK(out["a"].get<int>() == 1);
}

TEST_CASE("malformed JSON returns false") {
    njson out;
    CHECK_FALSE(PARSE_JSON("{not json}", out));
}

TEST_CASE("empty input returns false") {
    njson out;
    CHECK_FALSE(PARSE_JSON("", out));
}

} // ParseJson

// ===========================================================================
// GetJsonValue — scalar types, named vs unnamed access, missing/null handling
// ===========================================================================

TEST_SUITE("WPJson.GetJsonValue.Scalars") {

TEST_CASE("bool by name") {
    njson j = { { "flag", true } };
    bool  v = false;
    REQUIRE(GET_JSON_NAME_VALUE(j, "flag", v));
    CHECK(v);
}

TEST_CASE("int32_t by name") {
    njson   j = { { "n", 42 } };
    int32_t v = 0;
    REQUIRE(GET_JSON_NAME_VALUE(j, "n", v));
    CHECK(v == 42);
}

TEST_CASE("uint32_t by name") {
    njson    j = { { "n", 7u } };
    uint32_t v = 0;
    REQUIRE(GET_JSON_NAME_VALUE(j, "n", v));
    CHECK(v == 7u);
}

TEST_CASE("float by name") {
    njson j = { { "f", 3.5 } };
    float v = 0.f;
    REQUIRE(GET_JSON_NAME_VALUE(j, "f", v));
    CHECK(v == doctest::Approx(3.5f));
}

TEST_CASE("double by name") {
    njson  j = { { "d", 2.71828 } };
    double v = 0.;
    REQUIRE(GET_JSON_NAME_VALUE(j, "d", v));
    CHECK(v == doctest::Approx(2.71828));
}

TEST_CASE("std::string by name") {
    njson       j = { { "s", "hello" } };
    std::string v;
    REQUIRE(GET_JSON_NAME_VALUE(j, "s", v));
    CHECK(v == "hello");
}

TEST_CASE("missing key returns false (warn variant)") {
    njson       j = { { "present", 1 } };
    std::string v = "unchanged";
    CHECK_FALSE(GET_JSON_NAME_VALUE(j, "absent", v));
    CHECK(v == "unchanged");
}

TEST_CASE("missing key returns false (nowarn variant)") {
    njson       j = { { "present", 1 } };
    std::string v = "unchanged";
    CHECK_FALSE(GET_JSON_NAME_VALUE_NOWARN(j, "absent", v));
    CHECK(v == "unchanged");
}

TEST_CASE("null value returns false without overwrite") {
    njson j   = { { "k", nullptr } };
    int   v   = 99;
    CHECK_FALSE(GET_JSON_NAME_VALUE(j, "k", v));
    CHECK(v == 99);
}

TEST_CASE("type mismatch returns false without overwrite") {
    njson j = { { "k", "not a number" } };
    int   v = 55;
    CHECK_FALSE(GET_JSON_NAME_VALUE(j, "k", v));
    CHECK(v == 55);
}

TEST_CASE("unnamed form reads from root JSON") {
    njson j = 17;
    int   v = 0;
    REQUIRE(GET_JSON_VALUE(j, v));
    CHECK(v == 17);
}

} // Scalars

// ===========================================================================
// GetJsonValue — fixed-size arrays (uniform scalar replication + CSV string)
// ===========================================================================

TEST_SUITE("WPJson.GetJsonValue.Arrays") {

TEST_CASE("farray<3> from comma-separated string") {
    njson                j = { { "v", "1.0 2.0 3.0" } };
    std::array<float, 3> v = { 0, 0, 0 };
    REQUIRE(GET_JSON_NAME_VALUE(j, "v", v));
    CHECK(v[0] == doctest::Approx(1.0f));
    CHECK(v[1] == doctest::Approx(2.0f));
    CHECK(v[2] == doctest::Approx(3.0f));
}

TEST_CASE("farray<2> from CSV") {
    njson                j = { { "v", "0.5 1.5" } };
    std::array<float, 2> v { 0, 0 };
    REQUIRE(GET_JSON_NAME_VALUE(j, "v", v));
    CHECK(v[0] == doctest::Approx(0.5f));
    CHECK(v[1] == doctest::Approx(1.5f));
}

TEST_CASE("iarray<3> from CSV") {
    njson              j = { { "v", "10 20 30" } };
    std::array<int, 3> v { 0, 0, 0 };
    REQUIRE(GET_JSON_NAME_VALUE(j, "v", v));
    CHECK(v[0] == 10);
    CHECK(v[1] == 20);
    CHECK(v[2] == 30);
}

TEST_CASE("scalar number replicates across fixed-size array components") {
    njson                j = { { "scale", 2.5 } };
    std::array<float, 3> v { 0, 0, 0 };
    REQUIRE(GET_JSON_NAME_VALUE(j, "scale", v));
    CHECK(v[0] == doctest::Approx(2.5f));
    CHECK(v[1] == doctest::Approx(2.5f));
    CHECK(v[2] == doctest::Approx(2.5f));
}

TEST_CASE("vector<float> from CSV string") {
    njson              j = { { "v", "1 2 3 4" } };
    std::vector<float> v;
    REQUIRE(GET_JSON_NAME_VALUE(j, "v", v));
    REQUIRE(v.size() == 4);
    CHECK(v[0] == doctest::Approx(1.f));
    CHECK(v[3] == doctest::Approx(4.f));
}

TEST_CASE("vector<float> from scalar gets single-element vector") {
    njson              j = { { "v", 7.25 } };
    std::vector<float> v;
    REQUIRE(GET_JSON_NAME_VALUE(j, "v", v));
    REQUIRE(v.size() == 1);
    CHECK(v[0] == doctest::Approx(7.25f));
}

TEST_CASE("wrong-length CSV rejected by StrToArray") {
    njson                j = { { "v", "1 2" } }; // only 2 for size-3 target
    std::array<float, 3> v { 0, 0, 0 };
    // WrongSizeExp is caught internally and returns false
    CHECK_FALSE(GET_JSON_NAME_VALUE(j, "v", v));
}

} // Arrays

// ===========================================================================
// GetJsonValue — user property resolution via UserPropertiesScope
// ===========================================================================

TEST_SUITE("WPJson.GetJsonValue.UserProps") {

TEST_CASE("falls back to embedded 'value' when no scope active") {
    // {"user": "bright", "value": 0.7} with no UserPropertiesScope → default
    njson j     = { { "bright", { { "user", "bright" }, { "value", 0.7 } } } };
    float v     = 0.f;
    REQUIRE(GET_JSON_NAME_VALUE(j, "bright", v));
    CHECK(v == doctest::Approx(0.7f));
}

TEST_CASE("resolves from user properties when scope active") {
    WPUserProperties props;
    REQUIRE(props.LoadFromProjectJson(
        R"({"general":{"properties":{"bright":{"value":0.2,"type":"slider"}}}})"));
    UserPropertiesScope scope(&props);

    njson j = { { "bright", { { "user", "bright" }, { "value", 0.9 } } } };
    float v = 0.f;
    REQUIRE(GET_JSON_NAME_VALUE(j, "bright", v));
    CHECK(v == doctest::Approx(0.2f));
}

TEST_CASE("UserPropertiesScope nesting restores previous pointer") {
    WPUserProperties outer, inner;
    REQUIRE(outer.LoadFromProjectJson(
        R"({"general":{"properties":{"k":{"value":1.0}}}})"));
    REQUIRE(inner.LoadFromProjectJson(
        R"({"general":{"properties":{"k":{"value":2.0}}}})"));

    njson j = { { "k", { { "user", "k" }, { "value", 0.0 } } } };

    {
        UserPropertiesScope o(&outer);
        float               v = 0.f;
        GET_JSON_NAME_VALUE(j, "k", v);
        CHECK(v == doctest::Approx(1.0f));

        {
            UserPropertiesScope i(&inner);
            float               vi = 0.f;
            GET_JSON_NAME_VALUE(j, "k", vi);
            CHECK(vi == doctest::Approx(2.0f));
        }

        // After inner scope ends, outer is restored
        float v2 = 0.f;
        GET_JSON_NAME_VALUE(j, "k", v2);
        CHECK(v2 == doctest::Approx(1.0f));
    }

    // After outer scope ends, g_currentUserProperties is nullptr again —
    // so a default-form lookup falls back to embedded "value"
    njson j2 = { { "k", { { "user", "k" }, { "value", 42.0 } } } };
    float v3 = 0.f;
    GET_JSON_NAME_VALUE(j2, "k", v3);
    CHECK(v3 == doctest::Approx(42.0f));
}

TEST_CASE("non-object value returns as-is (ResolveUserProperty passthrough)") {
    // If the wrapped value isn't an object with "user", ResolveUserProperty is identity.
    njson j = 3.14;
    float v = 0.f;
    REQUIRE(GET_JSON_VALUE(j, v));
    CHECK(v == doctest::Approx(3.14f));
}

} // UserProps
