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

    // ---- Trailing-comma tolerance ----------------------------------------
    // Some wallpaper effect JSONs contain trailing commas before a closing
    // bracket (e.g. `,\n\t\t\t]`).  nlohmann 3.12 has `ignore_comments` but
    // not trailing-commas, so StripTrailingCommas pre-processes the source
    // inside ParseJson.

    TEST_CASE("trailing comma before ] is tolerated") {
        njson out;
        REQUIRE(PARSE_JSON("[1,2,3,]", out));
        REQUIRE(out.is_array());
        CHECK(out.size() == 3);
    }

    TEST_CASE("trailing comma before } is tolerated") {
        njson out;
        REQUIRE(PARSE_JSON(R"({"a":1,"b":2,})", out));
        CHECK(out["a"].get<int>() == 1);
        CHECK(out["b"].get<int>() == 2);
    }

    TEST_CASE("trailing comma with whitespace/newlines before ] is tolerated") {
        njson out;
        REQUIRE(PARSE_JSON("[\n  \"a\",\n  \"b\" ,\n\t\t\t]", out));
        REQUIRE(out.is_array());
        CHECK(out.size() == 2);
    }

    TEST_CASE("commas inside strings are preserved (not stripped)") {
        njson out;
        REQUIRE(PARSE_JSON(R"({"s":"hello, world,","n":1})", out));
        CHECK(out["s"].get<std::string>() == "hello, world,");
        CHECK(out["n"].get<int>() == 1);
    }

    TEST_CASE("escaped quote inside string doesn't break stripping") {
        njson out;
        // Source string is literally:  {"s":"a\",","n":1}
        // — the embedded \"  must not confuse the string-tracking state machine.
        REQUIRE(PARSE_JSON(R"({"s":"a\",","n":1})", out));
        CHECK(out["s"].get<std::string>() == "a\",");
        CHECK(out["n"].get<int>() == 1);
    }

    TEST_CASE("nested trailing commas tolerated") {
        njson out;
        REQUIRE(PARSE_JSON(R"({"a":[1,2,],"b":{"c":3,},})", out));
        CHECK(out["a"].size() == 2);
        CHECK(out["b"]["c"].get<int>() == 3);
    }

    // ---- Resource caps ---------------------------------------------------
    // Hard caps on input bytes, nesting depth, and total element count
    // protect plasmashell from a hostile scene.json wedging the parser
    // recursion stack or amplifying memory via deeply-nested / huge inputs.

    TEST_CASE("rejects JSON deeper than the depth cap without recursion blowup") {
        // Synthesize kMaxJsonDepth+1 nested arrays.  Pre-cap behaviour was a
        // recursive-descent parse that could SIGSEGV on the C stack at this
        // shape; post-cap, ParseJson returns false cleanly without recursion.
        constexpr std::size_t kOverDepth = wallpaper::kMaxJsonDepth + 1;
        std::string           deep(kOverDepth, '[');
        deep.append(kOverDepth, ']');
        njson out;
        CHECK_FALSE(PARSE_JSON(deep, out));
    }

    TEST_CASE("accepts JSON exactly at the depth cap") {
        constexpr std::size_t kAtDepth = wallpaper::kMaxJsonDepth;
        std::string           deep(kAtDepth, '[');
        deep.append(kAtDepth, ']');
        njson out;
        REQUIRE(PARSE_JSON(deep, out));
        CHECK(out.is_array());
    }

    TEST_CASE("rejects JSON with more elements than the element cap") {
        // Build a top-level array of kMaxJsonElements + 1 zeros.  Each digit
        // counts as one element under the SAX callback; the array itself
        // counts too, so the cap bites partway through.
        std::string s = "[0";
        s.reserve(wallpaper::kMaxJsonElements * 3);
        for (std::size_t i = 1; i < wallpaper::kMaxJsonElements + 1; ++i) s += ",0";
        s += ']';
        njson out;
        CHECK_FALSE(PARSE_JSON(s, out));
    }

    TEST_CASE("accepts JSON near the element cap") {
        // Build a top-level array of (kMaxJsonElements - 1) zeros — together
        // with the wrapping array itself, the total element count equals
        // kMaxJsonElements (the cap is inclusive: <= passes).
        std::string s = "[0";
        s.reserve(wallpaper::kMaxJsonElements * 3);
        for (std::size_t i = 2; i < wallpaper::kMaxJsonElements; ++i) s += ",0";
        s += ']';
        njson out;
        REQUIRE(PARSE_JSON(s, out));
        REQUIRE(out.is_array());
        CHECK(out.size() == wallpaper::kMaxJsonElements - 1);
    }

    TEST_CASE("rejects JSON larger than the byte cap before parse starts") {
        // Construct a string > kMaxJsonBytes that would otherwise parse fine.
        // Reserve before building so the test doesn't OOM on the resize.
        std::string oversize;
        oversize.reserve(wallpaper::kMaxJsonBytes + 16);
        oversize = R"({"k":")";
        oversize.append(wallpaper::kMaxJsonBytes + 1 - oversize.size() - 2, 'x');
        oversize.append(R"("})");
        njson out;
        CHECK_FALSE(PARSE_JSON(oversize, out));
    }

    TEST_CASE("accepts JSON just under the byte cap") {
        // Benign string at kMaxJsonBytes-1024 so we don't tank doctest mem.
        std::string near = R"({"k":")";
        near.reserve(wallpaper::kMaxJsonBytes);
        near.append(wallpaper::kMaxJsonBytes - 1024 - near.size() - 2, 'x');
        near.append(R"("})");
        REQUIRE(near.size() < wallpaper::kMaxJsonBytes);
        njson out;
        REQUIRE(PARSE_JSON(near, out));
        CHECK(out.contains("k"));
    }

} // ParseJson

TEST_SUITE("WPJson.StripTrailingCommas") {
    TEST_CASE("passes through well-formed JSON unchanged") {
        CHECK(StripTrailingCommas("[1,2,3]") == "[1,2,3]");
        CHECK(StripTrailingCommas(R"({"a":1,"b":2})") == R"({"a":1,"b":2})");
    }
    TEST_CASE("strips comma before ]") {
        CHECK(StripTrailingCommas("[1,2,3,]") == "[1,2,3]");
    }
    TEST_CASE("strips comma before } with whitespace") {
        CHECK(StripTrailingCommas(R"({"a":1, })") == R"({"a":1 })");
    }
    TEST_CASE("preserves commas inside strings") {
        const std::string src = R"("a,b,c,")";
        CHECK(StripTrailingCommas(src) == src);
    }
    TEST_CASE("handles backslash escapes inside strings") {
        // Input: "\\","x" — the first string ends at the unescaped close quote
        // (the \\ is a literal backslash, then the " closes).  The trailing
        // comma before ] (not present here) is the only thing that would get
        // stripped — verify the string boundaries are detected correctly.
        const std::string src = R"(["\\",])";
        CHECK(StripTrailingCommas(src) == R"(["\\"])");
    }
}

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
        njson j = { { "k", nullptr } };
        int   v = 99;
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
        njson j = { { "bright", { { "user", "bright" }, { "value", 0.7 } } } };
        float v = 0.f;
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
        REQUIRE(outer.LoadFromProjectJson(R"({"general":{"properties":{"k":{"value":1.0}}}})"));
        REQUIRE(inner.LoadFromProjectJson(R"({"general":{"properties":{"k":{"value":2.0}}}})"));

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

TEST_SUITE("WPJson.StripLeadingZeros") {
    // Driver: Astronaut (2530355779) workshop shader has
    //   uniform float u_userSpeed; // {"material":"Speed","default":1,"range":[0,01]}
    // — `01` is invalid JSON (leading zeros forbidden by RFC 8259) but WE's
    // parser tolerates it.  Without the strip, nlohmann::json bails with
    // "syntax error while parsing array - unexpected number literal" and the
    // entire uniform annotation is lost.

    TEST_CASE("passes well-formed numbers unchanged") {
        CHECK(StripLeadingZeros(R"([0,1,2,3])") == R"([0,1,2,3])");
        CHECK(StripLeadingZeros(R"({"range":[0.5, 10.0]})") == R"({"range":[0.5, 10.0]})");
        CHECK(StripLeadingZeros(R"(0)") == R"(0)");           // standalone zero kept
        CHECK(StripLeadingZeros(R"({"x":0})") == R"({"x":0})");
        CHECK(StripLeadingZeros(R"([0])") == R"([0])");
    }

    TEST_CASE("strips leading zero from non-decimal integer") {
        CHECK(StripLeadingZeros(R"([0,01])") == R"([0,1])");
        CHECK(StripLeadingZeros(R"([01, 02, 03])") == R"([1, 2, 3])");
        CHECK(StripLeadingZeros(R"({"a":007})") == R"({"a":7})");
    }

    TEST_CASE("preserves 0.x decimals (the leading zero is significant)") {
        CHECK(StripLeadingZeros(R"([0.5, 0.25])") == R"([0.5, 0.25])");
        CHECK(StripLeadingZeros(R"([0.01])") == R"([0.01])");
    }

    TEST_CASE("inside strings the digits are preserved verbatim") {
        CHECK(StripLeadingZeros(R"({"x":"01:02:03"})") == R"({"x":"01:02:03"})");
    }

    TEST_CASE("end-to-end recovery: Astronaut's [0,01] parses") {
        const std::string in =
            R"({"material":"Speed","default":1,"range":[0,01]})";
        njson result;
        REQUIRE(PARSE_JSON(in, result));
        REQUIRE(result.at("range").is_array());
        CHECK(result.at("range").at(0).get<int>() == 0);
        CHECK(result.at("range").at(1).get<int>() == 1);
    }
}

TEST_SUITE("WPJson.QuoteFirstKey") {
    // Pattern produced by a quirky Workshop serializer: an inline `options`
    // block's first key drops its opening `"`, leaving `{Foo":0,"Bar":1}`.
    // Found across 5+ wallpapers (Falling Deeper 3061226599, Floating Ducks
    // 3377132665, Lost Cat. 2 3356678415, The Blur 3562086244, Final Demons
    // 3216242451) in the 2026-05-15 mass audit.  Without recovery the entire
    // shader uniform/COMBO line fails to parse, the affected uniform never
    // registers, and downstream shader compilation breaks.

    TEST_CASE("passes well-formed JSON unchanged") {
        CHECK(QuoteFirstKey(R"({"a":1,"b":2})") == R"({"a":1,"b":2})");
        CHECK(QuoteFirstKey(R"({"options":{"Color":0,"UV":1}})") ==
              R"({"options":{"Color":0,"UV":1}})");
        CHECK(QuoteFirstKey(R"([1,2,3])") == R"([1,2,3])");
    }

    TEST_CASE("quotes first key missing leading \" after {") {
        CHECK(QuoteFirstKey(R"({Color":0,"UV":1})") == R"({"Color":0,"UV":1})");
    }

    TEST_CASE("recovers WE Workshop options blocks") {
        // Real bytes lifted from procedural_noise.frag in Falling Deeper's pkg.
        const std::string in =
            R"({"material":"Noise category","combo":"AA_CATEGORY","type":"options","default":0,"options":{Color":0,"UV":1}})";
        const std::string expected =
            R"({"material":"Noise category","combo":"AA_CATEGORY","type":"options","default":0,"options":{"Color":0,"UV":1}})";
        CHECK(QuoteFirstKey(in) == expected);
    }

    TEST_CASE("handles identifier with spaces and +") {
        // WE allows multi-word labels like "Noise + mirrored" as option keys.
        const std::string in       = R"({Noise + mirrored":2,"Noise + opacity mask":3})";
        const std::string expected = R"({"Noise + mirrored":2,"Noise + opacity mask":3})";
        CHECK(QuoteFirstKey(in) == expected);
    }

    TEST_CASE("does not touch identifiers inside string literals") {
        // A barewordish sequence appearing inside a string value must be preserved
        // exactly; the fixup only fires outside string context.
        const std::string in = R"({"name":"{Foo\":bar"})";
        CHECK(QuoteFirstKey(in) == in);
    }

    TEST_CASE("does not touch unquoted blocks that aren't `{ident\"` shape") {
        // Object with unquoted keys but no closing `"` — not our pattern, keep as-is.
        CHECK(QuoteFirstKey(R"({Color:0,UV:1})") == R"({Color:0,UV:1})");
        // Empty object
        CHECK(QuoteFirstKey(R"({})") == R"({})");
        // Object opening followed by a number (array-like)
        CHECK(QuoteFirstKey(R"({1:"a"})") == R"({1:"a"})");
    }

    TEST_CASE("end-to-end: shipped Workshop shader COMBO line parses") {
        // Verifies the full ParseJson pipeline (strip + quote + nlohmann)
        // accepts the broken-but-recoverable JSON shape.
        const std::string in =
            R"({"material":"Noise category","combo":"AA_CATEGORY","type":"options","default":0,"options":{Color":0,"UV":1}})";
        njson result;
        REQUIRE(PARSE_JSON(in, result));
        CHECK(result.at("combo").get<std::string>() == "AA_CATEGORY");
        REQUIRE(result.at("options").is_object());
        CHECK(result.at("options").at("Color").get<int>() == 0);
        CHECK(result.at("options").at("UV").get<int>() == 1);
    }

} // QuoteFirstKey

// ===========================================================================
// ResolveUserPropertyRef — copy-free resolution: returns a reference into the
// input for the common (no-"user" / no-context / embedded-default) paths, and
// only materializes a value into caller-owned storage for the genuine resolved
// path (so it cannot dangle).  Address-equality assertions are the proof that
// the common path performs no deep copy.
// ===========================================================================

TEST_SUITE("WPJson.ResolveUserPropertyRef") {

    TEST_CASE("no-user scalar returns a reference to the same object (no copy)") {
        njson                         input = 3.14;
        std::optional<njson>          storage;
        const njson&                  out = ResolveUserPropertyRef(input, storage);
        CHECK(&out == &input);              // same object: zero copy
        CHECK_FALSE(storage.has_value());   // resolved path never taken
        CHECK(out == 3.14);
    }

    TEST_CASE("no-user object returns a reference to the same object (no copy)") {
        njson                input   = { { "color", "red" }, { "n", 7 } };
        std::optional<njson> storage;
        const njson&         out = ResolveUserPropertyRef(input, storage);
        CHECK(&out == &input);
        CHECK_FALSE(storage.has_value());
        CHECK(out == input);
    }

    TEST_CASE("no-user array returns a reference to the same object (no copy)") {
        njson                input   = njson::array({ 1, 2, 3 });
        std::optional<njson> storage;
        const njson&         out = ResolveUserPropertyRef(input, storage);
        CHECK(&out == &input);
        CHECK_FALSE(storage.has_value());
        CHECK(out == input);
    }

    TEST_CASE("user field, no active context — embedded value default, still copy-free") {
        // g_currentUserProperties is nullptr (no scope), so the resolver falls
        // back to the embedded "value" — a reference INTO the input, not a copy.
        REQUIRE(g_currentUserProperties == nullptr);
        njson                input   = { { "user", "bright" }, { "value", 0.7 } };
        std::optional<njson> storage;
        const njson&         out = ResolveUserPropertyRef(input, storage);
        CHECK(&out == &input.at("value"));  // reference into the input subtree
        CHECK_FALSE(storage.has_value());
        CHECK(out == 0.7);
    }

    TEST_CASE("active context resolves a stored value into caller storage (no dangle)") {
        WPUserProperties props;
        REQUIRE(props.LoadFromProjectJson(
            R"({"general":{"properties":{"speed":{"value":5,"type":"slider"}}}})"));
        UserPropertiesScope scope(&props);

        njson                input   = { { "user", "speed" }, { "value", 1 } };
        std::optional<njson> storage;
        const njson&         out = ResolveUserPropertyRef(input, storage);
        CHECK(storage.has_value());     // materialized — this is the only copying path
        CHECK(&out == &*storage);       // reference points into caller-owned slot
        CHECK(out == 5);                // resolved value, not the embedded literal
    }

    TEST_CASE("active context synthesized bool lands in caller storage (no dangle)") {
        // Default-flip branch: combo prop at non-default value, bool-typed
        // binding -> ResolveValue synthesizes a fresh bool (a temporary).  It
        // must be parked in `storage`, never returned as a dangling reference.
        WPUserProperties props;
        REQUIRE(props.LoadFromProjectJson(
            R"({"general":{"properties":{"style":{"type":"combo","value":"6"}}}})"));
        props.SetProperty("style", "3"); // now differs from default "6"
        UserPropertiesScope scope(&props);

        njson                input   = { { "user", "style" }, { "value", true } };
        std::optional<njson> storage;
        const njson&         out = ResolveUserPropertyRef(input, storage);
        CHECK(storage.has_value());
        CHECK(&out == &*storage);
        CHECK(out == false);            // current != default -> !visDefault
    }

} // WPJson.ResolveUserPropertyRef

// ---------------------------------------------------------------------------
// Fuzz crash regression replay.
//
// Iterates tests/fixtures/fuzz_regressions/WPJsonParse/*.bin and feeds each
// file through the same entry point fuzz_WPJsonParse drives
// (wallpaper::ParseJson via the SAX-driven gate).
// ---------------------------------------------------------------------------

#include "test_data_root.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

TEST_SUITE("regression: minimised fuzz crashes") {
    TEST_CASE("regression: minimised fuzz crashes round-trip cleanly") {
        namespace fs2 = std::filesystem;
        const fs2::path dir = wallpaper::test::test_data_root()
                              / "fuzz_regressions" / "WPJsonParse";
        if (! fs2::exists(dir)) return;
        for (auto& entry : fs2::directory_iterator(dir)) {
            if (entry.path().extension() != ".bin") continue;
            SUBCASE(entry.path().filename().string().c_str()) {
                std::ifstream in(entry.path(), std::ios::binary);
                std::string src(std::istreambuf_iterator<char>(in), {});
                njson       out;
                CHECK_NOTHROW((void)wallpaper::ParseJson(
                    __FILE__, __FUNCTION__, __LINE__, src, out));
            }
        }
    }
}
