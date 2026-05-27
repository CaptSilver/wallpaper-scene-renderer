#include <doctest.h>
#include "WPShaderTransforms.h"
#include "WPShaderPreamble.hpp"
#include "Fs/VFS.h"
#include "WPShaderParser.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {
// Drives WPShaderParser::CompileToSpv against a single shader unit and
// returns the post-translation source plus the call's bool result.  Used
// by the volumetric end-to-end tests below.  No cache dir is mounted, so
// the compile takes the synchronous in-process branch.
struct CompileResult {
    bool        ok;            // CompileToSpv return value
    std::string post_xlat_src; // unit.src after preamble + clip rewrite + preprocess
};
inline CompileResult compileSingleShaderFromSource(wallpaper::ShaderType    stage,
                                                   const std::string&       src,
                                                   const wallpaper::Combos& combos) {
    static const bool initOnce = [] {
        wallpaper::WPShaderParser::InitGlslang();
        return true;
    }();
    (void)initOnce;

    wallpaper::fs::VFS                      vfs;   // no "cache" mount
    wallpaper::WPShaderInfo                 info;
    info.combos = combos;
    std::vector<wallpaper::WPShaderUnit>    units { { stage, src, {} } };
    std::vector<wallpaper::ShaderCode>      codes;
    std::vector<wallpaper::WPShaderTexInfo> texs;
    bool ok = wallpaper::WPShaderParser::CompileToSpv("test-end-to-end",
                                                     units,
                                                     codes,
                                                     vfs,
                                                     &info,
                                                     texs);
    return { ok, units[0].src };
}

// Reads the WE-install shader file by logical name.  Returns empty string
// if the install is not reachable (the test then SKIPs).
inline std::string readWeShipped(const char* logical_name) {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    std::string path = std::string(home) +
        "/.local/share/Steam/steamapps/common/wallpaper_engine/assets/shaders/" +
        logical_name;
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

// ===========================================================================
// regexTransformAll
// ===========================================================================

TEST_SUITE("regexTransformAll") {
    TEST_CASE("zero matches returns 0 and text unchanged") {
        std::string text = "hello world";
        std::regex  re("MISSING");
        int         count = regexTransformAll(text, re, [](const std::smatch&) {
            return std::string("X");
        });
        CHECK(count == 0);
        CHECK(text == "hello world");
    }

    TEST_CASE("single match at start") {
        std::string text = "AAA BBB";
        std::regex  re("AAA");
        int         count = regexTransformAll(text, re, [](const std::smatch&) {
            return std::string("XX");
        });
        CHECK(count == 1);
        CHECK(text == "XX BBB");
    }

    TEST_CASE("single match at end") {
        std::string text = "AAA BBB";
        std::regex  re("BBB");
        int         count = regexTransformAll(text, re, [](const std::smatch&) {
            return std::string("YY");
        });
        CHECK(count == 1);
        CHECK(text == "AAA YY");
    }

    TEST_CASE("single match in middle") {
        std::string text = "aXb";
        std::regex  re("X");
        int         count = regexTransformAll(text, re, [](const std::smatch&) {
            return std::string("---");
        });
        CHECK(count == 1);
        CHECK(text == "a---b");
    }

    TEST_CASE("two matches preserve inter-match text exactly") {
        std::string text = "prefixAAmiddleBBsuffix";
        std::regex  re("(AA|BB)");
        int         count = regexTransformAll(text, re, [](const std::smatch& m) {
            return "[" + m[0].str() + "]";
        });
        CHECK(count == 2);
        CHECK(text == "prefix[AA]middle[BB]suffix");
    }

    TEST_CASE("three adjacent matches") {
        std::string text = "aaa";
        std::regex  re("a");
        int         count = regexTransformAll(text, re, [](const std::smatch&) {
            return std::string("bb");
        });
        CHECK(count == 3);
        CHECK(text == "bbbbbb");
    }

    TEST_CASE("replacer uses match groups") {
        std::string text = "x=1; y=2; z=3;";
        std::regex  re("(\\w+)=(\\d+)");
        int         count = regexTransformAll(text, re, [](const std::smatch& m) {
            return m[1].str() + " is " + m[2].str();
        });
        CHECK(count == 3);
        CHECK(text == "x is 1; y is 2; z is 3;");
    }

    TEST_CASE("match at position 0 then later match") {
        std::string text = "AB_CD_EF";
        std::regex  re("(AB|EF)");
        int         count = regexTransformAll(text, re, [](const std::smatch& m) {
            return "[" + m[0].str() + "]";
        });
        CHECK(count == 2);
        CHECK(text == "[AB]_CD_[EF]");
    }

    TEST_CASE("replacement shorter than match") {
        std::string text = "longword short longword";
        std::regex  re("longword");
        int         count = regexTransformAll(text, re, [](const std::smatch&) {
            return std::string("s");
        });
        CHECK(count == 2);
        CHECK(text == "s short s");
    }

    TEST_CASE("replacement longer than match") {
        std::string text = "a b a";
        std::regex  re("a");
        int         count = regexTransformAll(text, re, [](const std::smatch&) {
            return std::string("XXXX");
        });
        CHECK(count == 2);
        CHECK(text == "XXXX b XXXX");
    }

} // TEST_SUITE regexTransformAll

// ===========================================================================
// findMatchingParen
// ===========================================================================

TEST_SUITE("findMatchingParen") {
    TEST_CASE("simple pair") { CHECK(findMatchingParen("(abc)", 0) == 5); }

    TEST_CASE("nested") { CHECK(findMatchingParen("(a(b)c)", 0) == 7); }

    TEST_CASE("deeply nested") { CHECK(findMatchingParen("(((x)))", 0) == 7); }

    TEST_CASE("returns npos for unmatched") {
        CHECK(findMatchingParen("(abc", 0) == std::string::npos);
    }

    TEST_CASE("inner open paren") {
        //  01234567
        //  xx(a(b))
        CHECK(findMatchingParen("xx(a(b))", 2) == 8);
    }

    TEST_CASE("empty parens") { CHECK(findMatchingParen("()", 0) == 2); }

    TEST_CASE("stops scanning after depth reaches 0 even if extra ')' follows") {
        // "())" — the match closes at index 2 (position after the first ')').
        // Mutation `depth > 0` → `depth >= 0` would let the loop consume the
        // second ')', taking depth negative and returning npos.  This test
        // pins the original semantics: stop scanning as soon as we're matched.
        CHECK(findMatchingParen("())", 0) == 2);
        CHECK(findMatchingParen("(xyz)abc)def", 0) == 5);
    }

    TEST_CASE("match at the very end of the string") {
        // The closing ')' is the last character; the return position equals
        // text.size(), so the outer `i < text.size()` guard must exit exactly
        // at size.  Mutation `i < size` → `i <= size` would read past the
        // terminator and misreport the match position.
        std::string s = "(abc)";
        CHECK(findMatchingParen(s, 0) == s.size());
    }

} // TEST_SUITE findMatchingParen

// ===========================================================================
// skipWhitespaceAndSemicolon
// ===========================================================================

TEST_SUITE("skipWhitespaceAndSemicolon") {
    TEST_CASE("no whitespace or semicolon") { CHECK(skipWhitespaceAndSemicolon("abc", 0) == 0); }

    TEST_CASE("spaces then semicolon") { CHECK(skipWhitespaceAndSemicolon("  ;rest", 0) == 3); }

    TEST_CASE("tab then semicolon") { CHECK(skipWhitespaceAndSemicolon("\t;x", 0) == 2); }

    TEST_CASE("just semicolon") { CHECK(skipWhitespaceAndSemicolon(";", 0) == 1); }

    TEST_CASE("at end of string") { CHECK(skipWhitespaceAndSemicolon("abc", 3) == 3); }

    TEST_CASE("spaces without semicolon") { CHECK(skipWhitespaceAndSemicolon("   x", 0) == 3); }

} // TEST_SUITE skipWhitespaceAndSemicolon

// ===========================================================================
// findEnclosingCallInfo
// ===========================================================================

TEST_SUITE("findEnclosingCallInfo") {
    TEST_CASE("simple function first arg") {
        // foo(X)  — X at position 4
        auto info = findEnclosingCallInfo("foo(X)", 4);
        CHECK(info.funcName == "foo");
        CHECK(info.argIndex == 0);
    }

    TEST_CASE("second arg after comma") {
        // func(a, X)  — X at position 8
        auto info = findEnclosingCallInfo("func(a, X)", 8);
        CHECK(info.funcName == "func");
        CHECK(info.argIndex == 1);
    }

    TEST_CASE("third arg") {
        // f(a, b, X)  — X at position 8
        auto info = findEnclosingCallInfo("f(a, b, X)", 8);
        CHECK(info.funcName == "f");
        CHECK(info.argIndex == 2);
    }

    TEST_CASE("nested paren in earlier arg") {
        // f(g(1,2), X) — commas inside g() don't count
        auto info = findEnclosingCallInfo("f(g(1,2), X)", 10);
        CHECK(info.funcName == "f");
        CHECK(info.argIndex == 1);
    }

    TEST_CASE("not inside a function call") {
        auto info = findEnclosingCallInfo("just text X", 10);
        CHECK(info.funcName == "");
        CHECK(info.argIndex == -1);
    }

    TEST_CASE("function name with underscores") {
        auto info = findEnclosingCallInfo("my_func(X)", 8);
        CHECK(info.funcName == "my_func");
    }

    TEST_CASE("space before paren") {
        auto info = findEnclosingCallInfo("func (X)", 6);
        CHECK(info.funcName == "func");
    }

} // TEST_SUITE findEnclosingCallInfo

// ===========================================================================
// NeedsFlatDecoration
// ===========================================================================

TEST_SUITE("NeedsFlatDecoration") {
    TEST_CASE("int type requires flat") { CHECK(NeedsFlatDecoration("in int v_Id;") == true); }

    TEST_CASE("uint type requires flat") {
        CHECK(NeedsFlatDecoration("out uint v_Flags;") == true);
    }

    TEST_CASE("ivec3 type requires flat") {
        CHECK(NeedsFlatDecoration("in ivec3 v_Indices;") == true);
    }

    TEST_CASE("uvec2 type requires flat") {
        CHECK(NeedsFlatDecoration("in uvec2 v_Pair;") == true);
    }

    TEST_CASE("float type does NOT need flat") {
        CHECK(NeedsFlatDecoration("in float v_Alpha;") == false);
    }

    TEST_CASE("vec4 type does NOT need flat") {
        CHECK(NeedsFlatDecoration("out vec4 v_Color;") == false);
    }

    TEST_CASE("already flat-qualified is skipped") {
        CHECK(NeedsFlatDecoration("flat in int v_Id;") == false);
    }

    TEST_CASE("flat after type keyword is skipped") {
        CHECK(NeedsFlatDecoration("in flat uint v_Flags;") == false);
    }

} // TEST_SUITE

// ===========================================================================
// FixImplicitConversions
// ===========================================================================

TEST_SUITE("FixImplicitConversions") {
    // --- Pattern 1: float VAR = int(...) → int VAR ---

    TEST_CASE("float var assigned int constructor becomes int var") {
        std::string in  = "float x = int(foo);";
        std::string out = "int x = int(foo);";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    TEST_CASE("float var with float assignment unchanged") {
        std::string in = "float x = 3.14;";
        CHECK_EQ(FixImplicitConversions(in), in);
    }

    // --- Pattern 2: uint modulo — 3 sub-patterns ---

    TEST_CASE("uint = (word + lit) % N") {
        std::string in  = "uint b = (a + 1) % 32;";
        std::string out = "uint b = uint((int(a) + 1) % 32);";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    TEST_CASE("uint = (word - lit) % N") {
        std::string in  = "uint b = (a - 3) % 16;";
        std::string out = "uint b = uint((int(a) - 3) % 16);";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    TEST_CASE("uint = word % N") {
        std::string in  = "uint idx = val % 8;";
        std::string out = "uint idx = uint(int(val) % 8);";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    TEST_CASE("general word % N") {
        std::string in  = "x = foo % 10;";
        std::string out = "x = int(foo) % 10;";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    // --- Pattern 3: vec2→vec4 upgrade for out-of-range swizzle ---

    TEST_CASE("vec2 upgraded to vec4 when .z accessed") {
        std::string in     = "vec2 tc;\nfloat z = tc.z;";
        auto        result = FixImplicitConversions(in);
        // Declaration should become vec4
        CHECK(result.find("vec4 tc;") != std::string::npos);
    }

    TEST_CASE("vec2 upgraded to vec4 when .w accessed") {
        std::string in     = "vec2 uv;\nfloat w = uv.w;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("vec4 uv;") != std::string::npos);
    }

    TEST_CASE("vec2 with only .xy access NOT upgraded") {
        std::string in     = "vec2 tc;\nfloat x = tc.x;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("vec2 tc;") != std::string::npos);
    }

    TEST_CASE("vec3 upgraded to vec4 when .w accessed") {
        std::string in     = "vec3 pos;\nfloat w = pos.w;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("vec4 pos;") != std::string::npos);
    }

    TEST_CASE("vec3 with only .xyz access NOT upgraded") {
        std::string in     = "vec3 pos;\nfloat z = pos.z;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("vec3 pos;") != std::string::npos);
    }

    TEST_CASE("vec2 upgraded to vec4 with texture coord fixup") {
        std::string in     = "vec2 tc;\nfloat z = tc.z;\nvec4 s = texture(g_Tex, tc);";
        auto        result = FixImplicitConversions(in);
        // After upgrade, texture() call should get .xy swizzle
        CHECK(result.find("texture(g_Tex, tc.xy)") != std::string::npos);
    }

    // --- Pattern 4: vec4→vec2, vec4→vec3, vec3→vec2 direct truncation ---

    TEST_CASE("vec2 = vec4 gets .xy swizzle") {
        std::string in     = "vec2 a;\nvec4 b;\na = b;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a = b.xy;") != std::string::npos);
    }

    TEST_CASE("vec3 = vec4 gets .xyz swizzle") {
        std::string in     = "vec3 a;\nvec4 b;\na = b;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a = b.xyz;") != std::string::npos);
    }

    TEST_CASE("vec2 = vec3 gets .xy swizzle") {
        std::string in     = "vec2 a;\nvec3 b;\na = b;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a = b.xy;") != std::string::npos);
    }

    TEST_CASE("same-type assignment unchanged") {
        std::string in     = "vec4 a;\nvec4 b;\na = b;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a = b;") != std::string::npos);
    }

    // --- Pattern 5: Arithmetic swizzle truncation ---

    TEST_CASE("vec2 var + expr.xyzw → truncate to .xy") {
        std::string in     = "vec2 a = vec2(0);\nvec4 b;\na + b.xyzw;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a + b.xy") != std::string::npos);
    }

    TEST_CASE("expr.xyz + vec2 var → truncate to .xy") {
        std::string in     = "vec2 a = vec2(0);\nvec3 b;\nb.xyz + a;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("b.xy + a") != std::string::npos);
    }

    TEST_CASE("vec2 var + expr.xyz → truncate 3 to 2") {
        std::string in     = "vec2 a = vec2(0);\nvec3 b;\na + b.xyz;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a + b.xy") != std::string::npos);
        CHECK(result.find("a + b.xyz") == std::string::npos);
    }

    TEST_CASE("truncation preserves surrounding code") {
        std::string in =
            "float z = 1.0;\nvec2 uv = vec2(0);\nvec4 c;\nfloat w = uv + c.xyzw;\nfloat q = 2.0;";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("uv + c.xy") != std::string::npos);
        CHECK(result.find("float z = 1.0") != std::string::npos);
        CHECK(result.find("float q = 2.0") != std::string::npos);
    }

    TEST_CASE("multiple truncations in same shader") {
        std::string in     = "vec2 a = vec2(0);\nvec4 b;\na + b.xyzw;\na - b.xyzw;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a + b.xy") != std::string::npos);
        CHECK(result.find("a - b.xy") != std::string::npos);
    }

    TEST_CASE("multiple truncations preserve inter-match text exactly") {
        // Tests that position arithmetic (position()-lastPos, position()+length())
        // correctly preserves text between matches. If mutated, second match
        // would corrupt the inter-match text.
        std::string in =
            "vec2 a = vec2(0);\nvec4 b;\nint x = 42; a + b.xyzw; int y = 99; a * b.xyz; int z = 7;";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("int x = 42; a + b.xy; int y = 99; a * b.xy; int z = 7;") !=
              std::string::npos);
    }

    TEST_CASE("reverse truncation preserves surrounding code") {
        std::string in     = "vec2 uv = vec2(0);\nvec4 c;\nfloat w = c.xyzw + uv;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("c.xy + uv") != std::string::npos);
    }

    TEST_CASE("vec2 with matching 2-component swizzle NOT truncated") {
        // sw > target_width loop: for vec2 (target=2), sw goes 4,3 — NOT 2
        // If mutated to >=, sw would also be 2, and .xy would be "truncated" to .xy (no change,
        // but the regex replacement would fire and potentially mangle the code)
        std::string in     = "vec2 a = vec2(0);\nvec4 b;\na + b.xy;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a + b.xy") != std::string::npos);
        // Must NOT have been further truncated to .x
        CHECK(result.find("a + b.x;") == std::string::npos);
    }

    TEST_CASE("vec3 with matching 3-component swizzle NOT truncated") {
        std::string in     = "vec3 a = vec3(0);\nvec4 b;\na + b.xyz;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a + b.xyz") != std::string::npos);
    }

    TEST_CASE("vec2 with 3-component swizzle truncated to 2") {
        std::string in     = "vec2 a = vec2(0);\nvec4 b;\na + b.xyz;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a + b.xy") != std::string::npos);
    }

    // --- Pattern 6: Arithmetic swizzle expansion ---

    TEST_CASE("vec3 var - expr.xx → expand to .xxx") {
        std::string in     = "vec3 a = vec3(0);\nvec4 b;\na - b.xx;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a - b.xxx") != std::string::npos);
    }

    TEST_CASE("expr.xy * vec4 var → expand to .xyyy") {
        std::string in     = "vec4 a = vec4(0);\nvec3 b;\nb.xy * a;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("b.xyyy * a") != std::string::npos);
    }

    TEST_CASE("expansion skipped when variable has swizzle suffix") {
        // If the vec3 var itself already has a swizzle (.xy), don't expand the RHS
        std::string in     = "vec3 a = vec3(0);\nvec4 b;\na.xy - b.xx;";
        auto        result = FixImplicitConversions(in);
        // Should NOT expand to .xxx since a.xy makes it a vec2 operation
        CHECK(result.find("b.xxx") == std::string::npos);
    }

    TEST_CASE("reverse expansion: expr.xx * vec3 → expr.xxx") {
        std::string in     = "vec3 a = vec3(0);\nvec4 b;\nb.xx * a;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("b.xxx * a") != std::string::npos);
    }

    TEST_CASE("expansion repeats last char correctly") {
        // vec4 var - expr.st → expand to .sttt (last char 't' repeated twice)
        std::string in     = "vec4 a = vec4(0);\nvec2 b;\na - b.st;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("a - b.sttt") != std::string::npos);
    }

    TEST_CASE("expansion preserves surrounding text exactly") {
        // This test checks substring extraction is exact — mutating position arithmetic corrupts
        // output
        std::string in =
            "vec3 myVar = vec3(1);\nvec4 other;\nfloat f = 1.0; myVar * other.xy; float g = 2.0;";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("myVar * other.xyy") != std::string::npos);
        CHECK(result.find("float f = 1.0") != std::string::npos);
        CHECK(result.find("float g = 2.0") != std::string::npos);
    }

    TEST_CASE("multiple expansions preserve inter-match text exactly") {
        std::string in =
            "vec3 a = vec3(0);\nvec4 b;\nint x = 1; a - b.xx; int y = 2; a + b.xy; int z = 3;";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("int x = 1; a - b.xxx; int y = 2; a + b.xyy; int z = 3;") !=
              std::string::npos);
    }

    TEST_CASE("expansion skip with swizzle then expand without — exact output") {
        // a.xy is a vec3 with existing swizzle → skip (no expansion)
        // a without swizzle → expand b.xx to b.xxx
        // Tests the vEnd position arithmetic: position() + m[1].length() for swizzle detection
        std::string in     = "vec3 a = vec3(0);\nvec4 b;\na.xy - b.xx; a + b.xx;";
        auto        result = FixImplicitConversions(in);
        // a.xy expression should NOT have b expanded (a.xy makes it vec2 context)
        CHECK(result.find("a.xy - b.xx") != std::string::npos);
        // bare a should have b expanded to .xxx
        CHECK(result.find("a + b.xxx") != std::string::npos);
    }

    TEST_CASE("expansion reverse skip: swizzled var then bare var exact output") {
        // Reverse direction: b.xx OP a.xy → skip (a has swizzle)
        //                    b.xx OP a    → expand to b.xxx OP a
        std::string in     = "vec3 a = vec3(0);\nvec4 b;\nb.xx * a.xy; b.xx + a;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("b.xx * a.xy") != std::string::npos); // skip
        CHECK(result.find("b.xxx + a") != std::string::npos);   // expand
    }

    TEST_CASE("expansion reverse: expr.xx OP vec3 exact output") {
        std::string in     = "vec3 a = vec3(0);\nvec4 b;\nb.xx + a;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("b.xxx + a") != std::string::npos);
        // Must NOT corrupt surrounding code
        CHECK(result.find("vec3 a = vec3(0)") != std::string::npos);
    }

    // --- Pattern 7: vec3 = mat * vec4() → .xyz ---

    TEST_CASE("vec3 = mat*vec4 gets .xyz truncation") {
        std::string in     = "vec3 pos = (g_ModelMatrix) * (vec4(v, 1.0)) ;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find(".xyz;") != std::string::npos);
    }

    TEST_CASE("vec4 = mat*vec4 unchanged") {
        std::string in     = "vec4 pos = (g_ModelMatrix) * (vec4(v, 1.0)) ;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find(".xyz") == std::string::npos);
    }

    // --- Pattern 8: pow in vecN constructor → pow(vecN, vecN) ---

    TEST_CASE("vec3(pow(x, y)) → pow(vec3(x), vec3(y))") {
        std::string in  = "vec3(pow(a, b))";
        std::string out = "pow(vec3(a), vec3(b))";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    TEST_CASE("vec2(pow(x, y)) → pow(vec2(x), vec2(y))") {
        std::string in  = "vec2(pow(a, b))";
        std::string out = "pow(vec2(a), vec2(b))";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    TEST_CASE("pow without vec wrapper unchanged") {
        std::string in = "pow(a, b)";
        CHECK_EQ(FixImplicitConversions(in), in);
    }

    // --- Pattern 9: float = texture() → .x ---

    TEST_CASE("float var = texture() gets .x swizzle") {
        std::string in  = "float val = texture(g_Texture0, uv);";
        std::string out = "float val = texture(g_Texture0, uv).x;";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    TEST_CASE("vec4 = texture() unchanged") {
        std::string in = "vec4 val = texture(g_Texture0, uv);";
        CHECK(FixImplicitConversions(in).find(".x;") == std::string::npos);
    }

    // --- Pattern 10: vec4 texture var * float uniform → .x at use sites ---

    TEST_CASE("vec4 texture var in float arithmetic gets .x") {
        std::string in = "uniform float u_Scale;\nvec4 timer = texture(g_Texture0, uv);\nfloat off "
                         "= u_Scale * timer;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("u_Scale * timer.x") != std::string::npos);
    }

    TEST_CASE("vec4 texture var with existing swizzle unchanged") {
        std::string in = "uniform float u_Scale;\nvec4 timer = texture(g_Texture0, uv);\nfloat off "
                         "= u_Scale * timer.r;";
        auto        result = FixImplicitConversions(in);
        // Should not double-swizzle
        CHECK(result.find("timer.r") != std::string::npos);
    }

    // --- Pattern 11: integer literal in ternary → bool() ---

    TEST_CASE("integer ternary condition gets bool() wrap") {
        std::string in  = "float x = 1 ? a : b;";
        std::string out = "float x = bool(1) ? a : b;";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    TEST_CASE("integer in ternary after ( gets bool() wrap") {
        std::string in     = "float x = (0 ? a : b);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("bool(0)") != std::string::npos);
    }

    TEST_CASE("non-integer ternary condition unchanged") {
        std::string in     = "float x = cond ? a : b;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("bool(") == std::string::npos);
    }

    // --- Pattern 11b: if(<float-expr>) → if(bool(<expr>)) ---
    // Driver: Game Of Life (3453251764) canvas chain — `if(u_mouseDown.x *
    // NOT(u_mouseDown.y))`, `if(u_enablePreview)`, `if(useLastAsNewUndoFrame)`.
    // HLSL accepts any nonzero numeric in `if`; GLSL requires bool.

    TEST_CASE("if(float_local) gets bool() wrap") {
        std::string in =
            "void main() {\n"
            "  float useLast = 1.0;\n"
            "  if (useLast) { }\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("if (bool(useLast))") != std::string::npos);
    }

    TEST_CASE("if(uniform_float) gets bool() wrap") {
        std::string in =
            "uniform float u_enablePreview;\n"
            "void main() {\n"
            "  if (u_enablePreview) { x = 1.0; }\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("if (bool(u_enablePreview))") != std::string::npos);
    }

    TEST_CASE("if(float_arith_expression) gets bool() wrap") {
        std::string in =
            "void main() {\n"
            "  if (u_mouseDown.x * NOT(u_mouseDown.y)) { x = 1.0; }\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("if (bool(u_mouseDown.x * NOT(u_mouseDown.y)))") !=
              std::string::npos);
    }

    TEST_CASE("if(comparison) left untouched") {
        std::string in =
            "void main() {\n"
            "  if (x > 0.5) { y = 1.0; }\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("bool(x > 0.5)") == std::string::npos);
        CHECK(result.find("if (x > 0.5)") != std::string::npos);
    }

    TEST_CASE("if(logical_and) left untouched") {
        std::string in =
            "void main() {\n"
            "  if (a < 1.0 && b > 0.0) { z = 1.0; }\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("bool(a < 1.0 && b > 0.0)") == std::string::npos);
    }

    TEST_CASE("if(equality) left untouched") {
        std::string in =
            "void main() {\n"
            "  if (mode == 1) { z = 1.0; }\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("bool(mode == 1)") == std::string::npos);
    }

    TEST_CASE("else if(float) gets bool() wrap") {
        std::string in =
            "void main() {\n"
            "  if (a > 0.0) { x = 1.0; }\n"
            "  else if (mask) { x = 2.0; }\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("else if (bool(mask))") != std::string::npos);
    }

    TEST_CASE("identifier ending in 'if' not matched") {
        std::string in =
            "void main() {\n"
            "  float endif_val = 1.0;\n"
            "  float y = endif_val + 1.0;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("bool(") == std::string::npos);
    }

    TEST_CASE("preprocessor #if left untouched") {
        std::string in =
            "#if MODIFIED_CURSOR_POS == 1\n"
            "vec4 cursor = u_mousePos;\n"
            "#endif\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("bool(") == std::string::npos);
        CHECK(result.find("#if MODIFIED_CURSOR_POS == 1") != std::string::npos);
    }

    TEST_CASE("already-wrapped if(bool(...)) not double-wrapped") {
        std::string in =
            "void main() {\n"
            "  if (bool(x)) { y = 1.0; }\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("bool(bool(x))") == std::string::npos);
    }

    // --- Pattern 11c: mix(vecN, scalar_float, ...) broadcast ---
    // Driver: Game Of Life canvas_pen_influence.frag —
    // `mix(vec2(0.,1.), influence, sameUiPart)` where `influence` is float.
    // HLSL's lerp() broadcasts scalars; GLSL's mix requires matching ranks.

    TEST_CASE("mix(vec2-literal, float-local, float) → vec LHS gets broadcast") {
        std::string in =
            "void main() {\n"
            "  float influence = 0.;\n"
            "  float sameUiPart = 1.0;\n"
            "  vec2 r;\n"
            "  r = mix(vec2(0., 1.), influence, sameUiPart);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("mix(vec2(0., 1.), vec2(influence),") != std::string::npos);
    }

    TEST_CASE("mix(vec2-literal, float-local, float) → float LHS truncates arg0") {
        // Game Of Life canvas_pen_influence pattern: `influence` is float,
        // assigned the result of mix() whose first arg is vec2.  WE's HLSL
        // truncates the vec result on assignment to float; we mimic by taking
        // `.x` of the vec2 literal.
        std::string in =
            "void main() {\n"
            "  float influence = 0.;\n"
            "  float sameUiPart = 1.0;\n"
            "  influence = mix(vec2(0., 1.), influence, sameUiPart);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("mix(vec2(0., 1.).x, influence,") != std::string::npos);
        CHECK(result.find("vec2(influence)") == std::string::npos);
    }

    TEST_CASE("mix(vec3-literal, float-local, float) → vec3 LHS broadcast to vec3") {
        std::string in =
            "void main() {\n"
            "  float gray = 0.5;\n"
            "  float t = 0.5;\n"
            "  vec3 r;\n"
            "  r = mix(vec3(0., 1., 0.), gray, t);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("mix(vec3(0., 1., 0.), vec3(gray),") != std::string::npos);
    }

    TEST_CASE("mix(vec2-literal, vec2-local, float) NOT broadcast") {
        std::string in =
            "void main() {\n"
            "  vec2 brushInfluence = vec2(1.0, 0.0);\n"
            "  float t = 0.5;\n"
            "  vec2 r = mix(vec2(0., 1.), brushInfluence, t);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        // Second arg is vec2 → broadcast must not fire
        CHECK(result.find("vec2(brushInfluence)") == std::string::npos);
    }

    TEST_CASE("mix(vec2-literal, unknown-ident, float) NOT broadcast") {
        // Defensive: if we can't prove the local is float, leave it alone.
        std::string in =
            "void main() {\n"
            "  vec2 r = mix(vec2(0., 1.), unknownVar, t);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("vec2(unknownVar)") == std::string::npos);
    }

    // --- Pattern 11d: mix(vec, vec, bool) → mix(..., ..., float(bool)) ---

    TEST_CASE("mix(vec, vec, bool_fn_call) wraps third arg with float()") {
        std::string in =
            "bool isMode(float a, float b) { return abs(a-b) < 0.5; }\n"
            "void main() {\n"
            "  vec2 mask = vec2(0., 0.);\n"
            "  vec2 r = mix(mask, vec2(1.0, 0.0), isMode(2., 2.));\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("float(isMode(2., 2.))") != std::string::npos);
    }

    TEST_CASE("mix(vec, vec, bool_local) wraps with float()") {
        std::string in =
            "void main() {\n"
            "  bool flag = true;\n"
            "  vec2 r = mix(vec2(0.), vec2(1.), flag);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("float(flag)") != std::string::npos);
    }

    TEST_CASE("mix(vec, vec, float_var) left alone") {
        std::string in =
            "void main() {\n"
            "  float t = 0.5;\n"
            "  vec2 r = mix(vec2(0.), vec2(1.), t);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("float(t)") == std::string::npos);
    }

    // --- Pattern 4 scope-ambiguity guard ---

    TEST_CASE("fixTrunc skips ambiguous names declared as both vec2 and vec4") {
        // Driver: Game Of Life canvas_paint.frag — `cursor` is a vec2 parameter
        // in calcStrokeInfluence AND a vec4 local in main(); the truncation
        // rewrite must not damage `vec4 cursor = u_mousePos;` in main().
        std::string in =
            "uniform vec4 u_mousePos;\n"
            "void calcStrokeInfluence(vec2 cursor) { float x = cursor.x; }\n"
            "void main() {\n"
            "  vec4 cursor = u_mousePos;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("vec4 cursor = u_mousePos;") != std::string::npos);
        CHECK(result.find("u_mousePos.xy") == std::string::npos);
    }

    TEST_CASE("fixTrunc still fires when name is unambiguously vec2") {
        // Sanity: a true `vec2 = vec4` truncation in a single-scope shader
        // still gets the .xy swizzle.
        std::string in =
            "uniform vec4 u_pos;\n"
            "void main() {\n"
            "  vec2 narrow;\n"
            "  narrow = u_pos;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("narrow = u_pos.xy;") != std::string::npos);
    }

    // --- Pattern 12: comparison in arithmetic → float() ---

    TEST_CASE("parenthesized comparison in arithmetic gets float() wrap") {
        std::string in  = "float x = (a > b) * c;";
        std::string out = "float x = float(a > b) * c;";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    TEST_CASE("comparison with <= gets float() wrap") {
        std::string in     = "float x = (a <= b) + d;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("float(a <= b)") != std::string::npos);
    }

    TEST_CASE("comparison without arithmetic unchanged") {
        std::string in     = "bool x = (a > b);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("float(a > b)") == std::string::npos);
    }

    // --- Pattern 13: bool var in compound assignment → float() ---

    TEST_CASE("bool var after *= gets float() wrap") {
        std::string in     = "bool flag = true;\nfloat x = 1.0;\nx *= flag;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("*= float(flag)") != std::string::npos);
    }

    TEST_CASE("bool var after * gets float() wrap") {
        std::string in     = "bool flag = true;\nfloat x = 1.0 * flag;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("* float(flag)") != std::string::npos);
    }

    TEST_CASE("non-bool var in compound assignment unchanged") {
        std::string in     = "float flag = 1.0;\nfloat x = 1.0;\nx *= flag;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("float(flag)") == std::string::npos);
    }

    // --- Pattern 14: int = step() → float ---

    TEST_CASE("int var assigned step() becomes float") {
        std::string in  = "int bar = step(0.5, x);";
        std::string out = "float bar = step(0.5, x);";
        CHECK_EQ(FixImplicitConversions(in), out);
    }

    TEST_CASE("int var with non-step assignment unchanged") {
        std::string in = "int bar = 42;";
        CHECK_EQ(FixImplicitConversions(in), in);
    }

    // --- Pattern 15: texture(sampler, vec4_varying) → .xy ---

    TEST_CASE("texture with vec4 varying gets .xy") {
        std::string in     = "in vec4 v_TexCoord;\nvec4 s = texture(g_Tex, v_TexCoord);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("texture(g_Tex, v_TexCoord.xy)") != std::string::npos);
    }

    TEST_CASE("texture with vec3 varying gets .xy") {
        std::string in     = "in vec3 v_UV;\nvec4 s = texture(g_Tex, v_UV);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("texture(g_Tex, v_UV.xy)") != std::string::npos);
    }

    TEST_CASE("texture with vec2 varying unchanged") {
        std::string in     = "in vec2 v_UV;\nvec4 s = texture(g_Tex, v_UV);";
        auto        result = FixImplicitConversions(in);
        // vec2 is correct size — the "in vec[34]" regex doesn't match vec2
        CHECK(result.find("texture(g_Tex, v_UV)") != std::string::npos);
    }

    // --- Pattern 15: multi-varying texture-coord fixup (N >= 3) ---
    //
    // Pins behaviour of the wide-varying loop: every "in vec3/vec4" varying
    // used bare in texture() gets .xy; vec2 varyings and varyings with an
    // existing swizzle / index are left alone; sampler arg is preserved verbatim
    // for each match.

    TEST_CASE("texture with 8 wide varyings each gets .xy exactly once") {
        std::string in =
            "in vec4 v0; in vec4 v1; in vec3 v2; in vec3 v3;\n"
            "in vec4 v4; in vec3 v5; in vec4 v6; in vec3 v7;\n"
            "void main() {\n"
            "  vec4 a = texture(g_T0, v0);\n"
            "  vec4 b = texture(g_T1, v1);\n"
            "  vec4 c = texture(g_T2, v2);\n"
            "  vec4 d = texture(g_T3, v3);\n"
            "  vec4 e = texture(g_T4, v4);\n"
            "  vec4 f = texture(g_T5, v5);\n"
            "  vec4 g = texture(g_T6, v6);\n"
            "  vec4 h = texture(g_T7, v7);\n"
            "}";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("texture(g_T0, v0.xy)") != std::string::npos);
        CHECK(result.find("texture(g_T1, v1.xy)") != std::string::npos);
        CHECK(result.find("texture(g_T2, v2.xy)") != std::string::npos);
        CHECK(result.find("texture(g_T3, v3.xy)") != std::string::npos);
        CHECK(result.find("texture(g_T4, v4.xy)") != std::string::npos);
        CHECK(result.find("texture(g_T5, v5.xy)") != std::string::npos);
        CHECK(result.find("texture(g_T6, v6.xy)") != std::string::npos);
        CHECK(result.find("texture(g_T7, v7.xy)") != std::string::npos);
        // Each varying must be rewritten exactly once — no .xy.xy double-swizzle.
        CHECK(result.find(".xy.xy") == std::string::npos);
    }

    TEST_CASE("mixed wide + vec2 + already-swizzled varyings — only bare wide get .xy") {
        std::string in =
            "in vec4 v_wide;\n"
            "in vec2 v_uv;\n"
            "in vec3 v_other;\n"
            "void main() {\n"
            "  vec4 a = texture(g_T0, v_wide);\n"          // rewrite
            "  vec4 b = texture(g_T1, v_uv);\n"            // skip (vec2)
            "  vec4 c = texture(g_T2, v_other.xy);\n"      // skip (already swizzled)
            "  vec4 d = texture(g_T3, v_other);\n"         // rewrite
            "}";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("texture(g_T0, v_wide.xy)") != std::string::npos);
        CHECK(result.find("texture(g_T1, v_uv)") != std::string::npos);
        CHECK(result.find("texture(g_T1, v_uv.xy)") == std::string::npos);
        CHECK(result.find("texture(g_T2, v_other.xy)") != std::string::npos);
        CHECK(result.find("texture(g_T2, v_other.xy.xy)") == std::string::npos);
        CHECK(result.find("texture(g_T3, v_other.xy)") != std::string::npos);
    }

    TEST_CASE("wide varying with .xyz / [i] suffix NOT double-swizzled") {
        // Verifies the spec's "negative case" note — the original regex
        // anchored on `<name>\s*\)`, i.e. only matched bare-name-then-close-paren.
        // The hoisted version must match the same shape via `(\w+)\s*\)`.
        std::string in =
            "in vec4 v_uv;\n"
            "void main() {\n"
            "  vec4 a = texture(g_T0, v_uv.xyz);\n"   // suffix → don't touch
            "  vec4 b = texture(g_T1, v_uv[0]);\n"    // index → don't touch
            "  vec4 c = texture(g_T2, v_uv);\n"       // bare → rewrite
            "}";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("texture(g_T0, v_uv.xyz)") != std::string::npos);
        CHECK(result.find("texture(g_T0, v_uv.xyz.xy)") == std::string::npos);
        CHECK(result.find("texture(g_T1, v_uv[0])") != std::string::npos);
        CHECK(result.find("texture(g_T1, v_uv[0].xy)") == std::string::npos);
        CHECK(result.find("texture(g_T2, v_uv.xy)") != std::string::npos);
    }

    TEST_CASE("texture call with sampler-arg only — no wide varyings → no-op") {
        // Empty wide_varyings set must early-out; result identical to input.
        std::string in =
            "in vec2 v_uv;\n"
            "void main() {\n"
            "  vec4 a = texture(g_T0, v_uv);\n"
            "}";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("texture(g_T0, v_uv)") != std::string::npos);
        CHECK(result.find(".xy)") == std::string::npos);
    }

    TEST_CASE("PERF: 8 wide varyings, 50 texture calls — record elapsed micros") {
        // Build a synthetic shader: 8 wide varyings, 50 texture() calls
        // distributing through them.  Pre-hoist this paid N x NFA build cost
        // per call; post-hoist pays 1x amortised across the run.
        std::string in = "in vec4 v0; in vec4 v1; in vec3 v2; in vec3 v3;\n"
                         "in vec4 v4; in vec3 v5; in vec4 v6; in vec3 v7;\n"
                         "void main() {\n";
        for (int i = 0; i < 50; ++i) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "  vec4 t%d = texture(g_T%d, v%d);\n",
                          i, i % 8, i % 8);
            in += buf;
        }
        in += "}";

        const auto t0 = std::chrono::steady_clock::now();
        const int  iterations = 20;
        for (int i = 0; i < iterations; ++i) {
            auto result = FixImplicitConversions(in);
            // Defeat dead-code elimination — assert on something that must hold.
            REQUIRE(result.find("texture(g_T0, v0.xy)") != std::string::npos);
        }
        const auto t1     = std::chrono::steady_clock::now();
        const auto micros =
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        MESSAGE("FixImplicitConversions 8-varying / 50-call * "
                << iterations << " iter: " << micros << " us total ("
                << (micros / iterations) << " us avg)");

        // Sanity floor only — guards against a future regression that turns
        // this back into per-name NFA rebuild.  Generous bound (10 s) to
        // absorb debug-build + libstdc++ ECMAScript NFA-build cost variability
        // across distrobox / lavapipe / native runs.
        CHECK(micros < 10'000'000);
    }

    // --- Pattern 16: const TYPE = texture() → remove const ---

    TEST_CASE("const vec4 = texture() removes const") {
        std::string in     = "const vec4 col = texture(g_Tex, uv);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("const") == std::string::npos);
        CHECK(result.find("vec4 col = texture(g_Tex, uv)") != std::string::npos);
    }

    TEST_CASE("const float = texture() removes const") {
        std::string in     = "const float val = texture(g_Tex, uv);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("const") == std::string::npos);
    }

    TEST_CASE("const with non-texture unchanged") {
        std::string in = "const vec4 col = vec4(1.0);";
        CHECK_EQ(FixImplicitConversions(in), in);
    }

    // --- Pattern 17: in-varying mutation → mutable copy ---

    TEST_CASE("in varying with compound assignment gets mutable copy") {
        std::string in     = "in vec2 v_TexCoord;\nvoid main() {\nv_TexCoord += offset;\n}";
        auto        result = FixImplicitConversions(in);
        // Should still have the 'in' declaration
        CHECK(result.find("in vec2 v_TexCoord;") != std::string::npos);
        // Should have a mutable copy
        CHECK(result.find("_m_v_TexCoord") != std::string::npos);
        CHECK(result.find("_m_v_TexCoord = v_TexCoord") != std::string::npos);
    }

    TEST_CASE("in varying without mutation unchanged") {
        std::string in =
            "in vec2 v_TexCoord;\nvoid main() {\nvec4 s = texture(g_Tex, v_TexCoord);\n}";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("_m_v_TexCoord") == std::string::npos);
    }

    TEST_CASE("in varying with plain assignment gets mutable copy") {
        std::string in     = "in vec2 v_TexCoord;\nvoid main() {\nv_TexCoord = v_TexCoord.yx;\n}";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("in vec2 v_TexCoord;") != std::string::npos);
        CHECK(result.find("_m_v_TexCoord") != std::string::npos);
        CHECK(result.find("_m_v_TexCoord = v_TexCoord") != std::string::npos);
    }

    TEST_CASE("in varying with component assignment gets mutable copy") {
        std::string in     = "in vec2 v_TexCoord;\nvoid main() {\nv_TexCoord.y = 1.0;\n}";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("_m_v_TexCoord") != std::string::npos);
    }

    TEST_CASE("in varying shadowed by local declaration not renamed") {
        // Regression: wallpaper 2866203962 chromatic_aberration had
        //   in vec4 rValue;
        //   void main() { vec4 rValue = texture(g_Tex, ...); }
        // Earlier we mistook the local declaration for an assignment-to-varying,
        // renamed all rValue → _m_rValue AND inserted a top-of-main mutable copy.
        // Result: two `vec4 _m_rValue` decls -> redefinition compile error.
        std::string in     = "in vec4 rValue;\n"
                             "void main() {\n"
                             "vec4 rValue = texture(g_Tex, v_TexCoord);\n"
                             "color = rValue;\n"
                             "}\n";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("_m_rValue") == std::string::npos);
        CHECK(result.find("in vec4 rValue;") != std::string::npos);
    }

    TEST_CASE("in varying with local shadow AND true assignment still not renamed") {
        // Local shadow takes precedence — we can't safely rename because the user
        // already chose to introduce their own local.  Accept the shadow.
        std::string in     = "in float threshold;\n"
                             "void main() {\n"
                             "float threshold = 0.5;\n"
                             "threshold += 0.1;\n"
                             "}\n";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("_m_threshold") == std::string::npos);
    }

    // --- Pattern 18: vec3 = vec4() → .xyz ---

    TEST_CASE("vec3 = vec4 constructor gets .xyz") {
        std::string in     = "vec3 pos = vec4(1.0, 2.0, 3.0, 4.0);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find(".xyz;") != std::string::npos);
    }

    TEST_CASE("vec4 = vec4 constructor unchanged") {
        std::string in     = "vec4 pos = vec4(1.0, 2.0, 3.0, 4.0);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find(".xyz") == std::string::npos);
    }

    // --- Pattern 19: float = vec_uniform * expr → .x ---

    TEST_CASE("float = vec_uniform * expr gets .x") {
        std::string in     = "uniform vec3 u_Color;\nfloat x = u_Color * 2.0;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("u_Color.x") != std::string::npos);
    }

    TEST_CASE("float = expr * vec_uniform gets .x") {
        std::string in     = "uniform vec4 u_Offset;\nfloat y = 3.0 * u_Offset;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("u_Offset.x;") != std::string::npos);
    }

    TEST_CASE("vec2 assignment with trailing vec uniform NOT wrongly .x'd") {
        // Regression: re2 used to match any "OP VEC_NAME;" regardless of LHS type,
        // incorrectly appending .x to a vec2 uniform in a vec2-typed statement
        // (breaks wallpaper 3276911872 clipping_mask effect).
        std::string in     = "uniform vec2 u_textureOffset;\nvec2 uvTex = foo - u_textureOffset;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("u_textureOffset.x;") == std::string::npos);
        CHECK(result.find("u_textureOffset;") != std::string::npos);
    }

    // --- Pattern 19b: wider varying truncated in narrower assignment ---

    TEST_CASE("vec4 varying truncated to .xy in vec2 assignment arithmetic") {
        std::string in     = "in vec4 v_TexCoord;\nuniform vec2 u_c;\n"
                             "vec2 uv = v_TexCoord * 2.0 - u_c;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("v_TexCoord.xy") != std::string::npos);
    }

    TEST_CASE("vec4 varying NOT truncated inside vec4 assignment") {
        std::string in     = "in vec4 v_TexCoord;\nvec4 r = v_TexCoord * 2.0;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("v_TexCoord.xy") == std::string::npos);
    }

    TEST_CASE("vec4 varying NOT truncated when passed bare to function") {
        // v_TexCoord is a bare arg to someFunc — must not add .xy (callee may want vec4).
        std::string in     = "in vec4 v_TexCoord;\nvec2 r = someFunc(v_TexCoord);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("someFunc(v_TexCoord)") != std::string::npos);
        CHECK(result.find("v_TexCoord.xy") == std::string::npos);
    }

    TEST_CASE("vec4 varying truncated when preceded by arithmetic op in vec2 assign") {
        std::string in     = "in vec4 v_TexCoord;\nvec2 uv = 1.0 - v_TexCoord;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("- v_TexCoord.xy") != std::string::npos);
    }

    TEST_CASE("vec4 varying + vec2 varying inside texture() gets .xy") {
        // Regression: clipping_mask.frag line 470 —
        //   texture(g_Texture1, v_TexCoord + v_ParallaxOffset * u_textureDepth)
        // v_TexCoord (vec4) adjacent to v_ParallaxOffset (vec2) must truncate.
        std::string in =
            "in vec4 v_TexCoord;\n"
            "in vec2 v_ParallaxOffset;\n"
            "uniform vec2 u_textureDepth;\n"
            "vec4 clip = texture(g_Texture1, v_TexCoord + v_ParallaxOffset * u_textureDepth);";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("v_TexCoord.xy + v_ParallaxOffset") != std::string::npos);
    }

    TEST_CASE("vec2 var + vec4 varying symmetry — varying gets .xy") {
        std::string in     = "in vec4 v_TexCoord;\nuniform vec2 u_c;\n"
                             "vec2 r = u_c + v_TexCoord;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("u_c + v_TexCoord.xy") != std::string::npos);
    }

    // --- Pattern 20: for-loop float-to-int ---

    TEST_CASE("for loop int var from float expr gets int() cast") {
        std::string in     = "for (int i = -u_Count * 2; i <= u_Count * 2; i++)";
        auto        result = FixImplicitConversions(in);
        bool        found  = result.find("int(-u_Count * 2)") != std::string::npos ||
                     result.find("int(- u_Count * 2)") != std::string::npos;
        CHECK(found);
    }

    TEST_CASE("for loop condition gets int() cast") {
        std::string in     = "i <= u_Count * 2;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("int(u_Count * 2)") != std::string::npos);
    }

    // --- Pattern 21: scalar assigned to glOutColor → vec4() ---

    TEST_CASE("scalar expression assigned to glOutColor gets vec4() wrap") {
        std::string in     = "glOutColor = sample.x * mask;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("glOutColor = vec4(sample.x * mask)") != std::string::npos);
    }

    TEST_CASE("vec4() already on glOutColor unchanged") {
        std::string in = "glOutColor = vec4(1.0, 0.0, 0.0, 1.0);";
        CHECK_EQ(FixImplicitConversions(in), in);
    }

    TEST_CASE("glOutColor component write not wrapped") {
        std::string in     = "glOutColor.a *= mask;";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("vec4(") == std::string::npos);
    }

    TEST_CASE("mix vec4 arg truncated to .rgb when LHS is .rgb") {
        std::string in     = "albedo.rgb = mix(albedo, hsv2rgb(colors), mask);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("mix(albedo.rgb,") != std::string::npos);
    }

    TEST_CASE("mix vec4 arg truncated to .xyz when LHS is .xyz") {
        std::string in     = "color.xyz = mix(color, other_vec3, t);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("mix(color.xyz,") != std::string::npos);
    }

    TEST_CASE("mix already swizzled — no double swizzle") {
        std::string in     = "albedo.rgb = mix(albedo.rgb, hsv2rgb(colors), mask);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("mix(albedo.rgb,") != std::string::npos);
        CHECK(result.find("albedo.rgb.rgb") == std::string::npos);
    }

    TEST_CASE("mix different var on LHS — unchanged") {
        std::string in     = "result.rgb = mix(albedo, hsv2rgb(colors), mask);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("mix(albedo,") != std::string::npos);
    }

    // --- Pattern: const qualifier on function return type ---

    TEST_CASE("const on function return type stripped") {
        // Note: fract is also a GLSL builtin, so the builtin-shadowing rename
        // pass also kicks in and renames to _w_fract.
        std::string in     = "const float fract(float x) {\n return x - floor(x);\n}";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("float _w_fract(float x)") != std::string::npos);
        CHECK(result.find("const float") == std::string::npos);
    }

    TEST_CASE("const on non-shadowing function return type stripped (no rename)") {
        std::string in     = "const vec3 myHelper(vec3 a) { return a; }";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("vec3 myHelper(vec3 a)") != std::string::npos);
        CHECK(result.find("const vec3") == std::string::npos);
    }

    TEST_CASE("const on vec3 function return stripped") {
        std::string in     = "const vec3 mymix(vec3 a, vec3 b) { return a * b; }";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("vec3 mymix(") != std::string::npos);
        CHECK(result.find("const vec3 mymix(") == std::string::npos);
    }

    TEST_CASE("const variable decl NOT touched") {
        std::string in = "const float PI = 3.14159;";
        CHECK_EQ(FixImplicitConversions(in), in);
    }

    TEST_CASE("const vec3 variable decl NOT touched") {
        std::string in = "const vec3 LumCoeff = vec3(0.2125, 0.7154, 0.0721);";
        CHECK_EQ(FixImplicitConversions(in), in);
    }

    // --- Pattern: user function with vecN param called with wider varying ---

    TEST_CASE("shimmer regression: rotateVec2(v_TexCoord) gets .xy") {
        std::string in     = "in vec4 v_TexCoord;\n"
                             "vec2 rotateVec2(vec2 v, float r) { return v; }\n"
                             "void main() {\n"
                             "  vec2 c = rotateVec2(v_TexCoord, 1.57);\n"
                             "}\n";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("rotateVec2(v_TexCoord.xy,") != std::string::npos);
    }

    TEST_CASE("user function vec3 param called with vec4 varying gets .xyz") {
        std::string in     = "in vec4 v_Color;\n"
                             "vec3 desaturate(vec3 c, float s) { return c; }\n"
                             "void main() {\n"
                             "  vec3 d = desaturate(v_Color, 0.5);\n"
                             "}\n";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("desaturate(v_Color.xyz,") != std::string::npos);
    }

    TEST_CASE("matching-width varying call NOT touched") {
        std::string in     = "in vec2 v_UV;\n"
                             "vec2 rotateVec2(vec2 v, float r) { return v; }\n"
                             "void main() {\n"
                             "  vec2 c = rotateVec2(v_UV, 1.57);\n"
                             "}\n";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("rotateVec2(v_UV,") != std::string::npos);
        CHECK(result.find("v_UV.xy") == std::string::npos);
    }

    TEST_CASE("already-swizzled varying arg NOT double-swizzled") {
        std::string in     = "in vec4 v_TexCoord;\n"
                             "vec2 rotateVec2(vec2 v, float r) { return v; }\n"
                             "void main() {\n"
                             "  vec2 c = rotateVec2(v_TexCoord.xy, 1.57);\n"
                             "}\n";
        auto        result = FixImplicitConversions(in);
        // Should not insert another .xy after the existing one
        CHECK(result.find("v_TexCoord.xy.xy") == std::string::npos);
        CHECK(result.find("rotateVec2(v_TexCoord.xy,") != std::string::npos);
    }

    TEST_CASE("unknown function (builtin) call NOT touched") {
        std::string in     = "in vec4 v_TexCoord;\n"
                             "void main() {\n"
                             "  vec4 c = texture(g_Texture0, v_TexCoord);\n"
                             "}\n";
        auto        result = FixImplicitConversions(in);
        // texture() is handled by the dedicated earlier pass; our new pass doesn't
        // know texture's signature because it's not declared in the shader body.
        // (The earlier pass already produces v_TexCoord.xy.)
        CHECK(result.find("texture(g_Texture0, v_TexCoord.xy)") != std::string::npos);
    }

    // --- Pattern: user function shadowing a GLSL builtin gets renamed ---

    TEST_CASE("user-defined fract renamed to _w_fract at def + calls") {
        std::string in     = "float fract(float x) {\n"
                             "  return x - floor(x);\n"
                             "}\n"
                             "void main() {\n"
                             "  float a = fract(1.5);\n"
                             "}\n";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("float _w_fract(float x)") != std::string::npos);
        CHECK(result.find("_w_fract(1.5)") != std::string::npos);
        // Original bare fract at call site should be gone
        CHECK(result.find(" fract(") == std::string::npos);
    }

    TEST_CASE("no user fract definition → builtin calls unchanged") {
        std::string in     = "void main() {\n  float a = fract(1.5);\n}";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("fract(1.5)") != std::string::npos);
        CHECK(result.find("_w_fract") == std::string::npos);
    }

    // --- Pattern: vec3 VAR = texture(...) gets .xyz ---

    TEST_CASE("vec3 assigned texture() gets .xyz") {
        std::string in     = "vec3 col = texture(g_Tex, uv);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("texture(g_Tex, uv).xyz;") != std::string::npos);
    }

    TEST_CASE("vec3 assigned texture() with nested call gets .xyz") {
        std::string in     = "vec3 col = texture(g_Tex, fract(uv));";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("texture(g_Tex, fract(uv)).xyz;") != std::string::npos);
    }

    TEST_CASE("vec3 assigned textureLod also handled") {
        std::string in     = "vec3 col = textureLod(g_Tex, uv, 0.0);";
        auto        result = FixImplicitConversions(in);
        CHECK(result.find("textureLod(g_Tex, uv, 0.0).xyz;") != std::string::npos);
    }

    TEST_CASE("vec4 VAR = texture() NOT touched (no truncation needed)") {
        std::string in     = "vec4 col = texture(g_Tex, uv);";
        auto        result = FixImplicitConversions(in);
        // Must not get .xyz
        CHECK(result.find(".xyz") == std::string::npos);
    }

} // TEST_SUITE("FixImplicitConversions")

// ===========================================================================
// FixEffectAlpha
// ===========================================================================

TEST_SUITE("FixEffectAlpha") {
    TEST_CASE("full assignment → unchanged") {
        std::string in = "void main() {\nglOutColor = vec4(1.0);\n}";
        CHECK_EQ(FixEffectAlpha(in), in);
    }

    TEST_CASE("no glOutColor at all → unchanged") {
        std::string in = "void main() {\nvec4 col = texture(g_Tex, uv);\n}";
        CHECK_EQ(FixEffectAlpha(in), in);
    }

    TEST_CASE("explicit alpha write (.a) → unchanged") {
        std::string in = "void main() {\nglOutColor.rgb = vec3(1.0);\nglOutColor.a = 1.0;\n}";
        CHECK_EQ(FixEffectAlpha(in), in);
    }

    TEST_CASE("explicit alpha write (.rgba) → unchanged") {
        std::string in = "void main() {\nglOutColor.rgba = vec4(1.0);\n}";
        CHECK_EQ(FixEffectAlpha(in), in);
    }

    TEST_CASE("explicit alpha write (.xyzw) → unchanged") {
        std::string in = "void main() {\nglOutColor.xyzw = vec4(1.0);\n}";
        CHECK_EQ(FixEffectAlpha(in), in);
    }

    TEST_CASE("component write without alpha → injects alpha preservation") {
        std::string in     = "void main() {\nglOutColor.rgb = vec3(1.0);\n}";
        auto        result = FixEffectAlpha(in);
        CHECK(result.find("glOutColor.a = texSample2D(g_Texture0, v_TexCoord.xy).a;") !=
              std::string::npos);
    }

    TEST_CASE("component .r write without alpha → injects alpha") {
        std::string in     = "void main() {\nglOutColor.r = 0.5;\n}";
        auto        result = FixEffectAlpha(in);
        CHECK(result.find("glOutColor.a = texSample2D") != std::string::npos);
    }

    TEST_CASE("injection placed before closing brace") {
        std::string in        = "void main() {\nglOutColor.rgb = vec3(1.0);\n}";
        auto        result    = FixEffectAlpha(in);
        auto        alpha_pos = result.find("glOutColor.a = texSample2D");
        auto        brace_pos = result.rfind('}');
        CHECK(alpha_pos != std::string::npos);
        CHECK(alpha_pos < brace_pos);
    }

} // TEST_SUITE("FixEffectAlpha")

// ===========================================================================
// FixCombineAlpha
// ===========================================================================

TEST_SUITE("FixCombineAlpha") {
    TEST_CASE("shine_combine pattern removed (macro-expanded saturate)") {
        // After macro expansion: saturate(x) → (clamp(x, 0.0, 1.0))
        std::string in     = "void main() {\n"
                             " albedo.a = (clamp(albedo.a + rays.a, 0.0, 1.0));\n"
                             " glOutColor = albedo;\n"
                             "}";
        auto        result = FixCombineAlpha(in);
        CHECK(result.find("// albedo.a") != std::string::npos);
        CHECK(result.find("glOutColor = albedo;") != std::string::npos);
    }

    TEST_CASE("godrays_combine with extra whitespace") {
        std::string in     = "void main() {\n"
                             " albedo.a = (clamp( albedo.a + rays.a , 0.0 , 1.0 )) ;\n"
                             " glOutColor = albedo;\n"
                             "}";
        auto        result = FixCombineAlpha(in);
        CHECK(result.find("// albedo.a") != std::string::npos);
    }

    TEST_CASE("fluidsimulation_combine NOT matched (different LHS/operand)") {
        std::string in     = "void main() {\n"
                             " albedo.a = (clamp(prev.a + albedo.a, 0.0, 1.0));\n"
                             " glOutColor = albedo;\n"
                             "}";
        auto        result = FixCombineAlpha(in);
        // Should be unchanged — prev != albedo on LHS
        CHECK_EQ(result, in);
    }

    TEST_CASE("no matching pattern → unchanged") {
        std::string in = "void main() {\n"
                         "  albedo.a = 1.0;\n"
                         "  glOutColor = albedo;\n"
                         "}";
        CHECK_EQ(FixCombineAlpha(in), in);
    }

    TEST_CASE("different variable names work") {
        std::string in     = "void main() {\n"
                             " color.a = (clamp(color.a + bloom.a, 0.0, 1.0));\n"
                             " glOutColor = color;\n"
                             "}";
        auto        result = FixCombineAlpha(in);
        CHECK(result.find("// color.a") != std::string::npos);
    }

} // TEST_SUITE("FixCombineAlpha")

// ===========================================================================
// TranslateGeometryShader
// ===========================================================================

TEST_SUITE("TranslateGeometryShader") {
    // --- Pattern 1: [maxvertexcount(N)] → #define + removal ---

    TEST_CASE("maxvertexcount extracted to #define") {
        std::string in     = "[maxvertexcount(6)]\nvoid main() {}";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("#define WE_GS_MAX_VERTICES (6)") != std::string::npos);
        CHECK(result.find("[maxvertexcount") == std::string::npos);
    }

    TEST_CASE("maxvertexcount with expression") {
        std::string in     = "[maxvertexcount(SEGMENTS*2+2)]\nvoid main() {}";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("#define WE_GS_MAX_VERTICES (SEGMENTS*2+2)") != std::string::npos);
    }

    TEST_CASE("no maxvertexcount uses fallback 4") {
        std::string in     = "void main() {}";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("#define WE_GS_MAX_VERTICES (4)") != std::string::npos);
    }

    // --- Pattern 2: gl_Position declaration removal ---

    TEST_CASE("in vec4 gl_Position removed") {
        std::string in     = "in vec4 gl_Position;\nvoid main() {}";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("in vec4 gl_Position;") == std::string::npos);
        CHECK(result.find("gl_Position input via gl_in") != std::string::npos);
    }

    TEST_CASE("out vec4 gl_Position removed") {
        std::string in     = "out vec4 gl_Position;\nvoid main() {}";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("out vec4 gl_Position;") == std::string::npos);
        CHECK(result.find("gl_Position output is built-in") != std::string::npos);
    }

    // --- Pattern 3: in TYPE v_xxx → in TYPE gs_in_v_xxx[] ---

    TEST_CASE("input varying renamed to gs_in_ array") {
        std::string in     = "in vec4 v_Color;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("in vec4 gs_in_v_Color[];") != std::string::npos);
    }

    TEST_CASE("input float varying renamed") {
        std::string in     = "in float v_Alpha;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("in float gs_in_v_Alpha[];") != std::string::npos);
    }

    TEST_CASE("output varying NOT renamed") {
        std::string in     = "out vec4 v_Color;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("gs_in_v_Color") == std::string::npos);
    }

    // --- Pattern 4: IN[0].gl_Position → gl_in[0].gl_Position ---

    TEST_CASE("IN[0].gl_Position → gl_in[0].gl_Position") {
        std::string in     = "vec4 pos = IN[0].gl_Position;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("gl_in[0].gl_Position") != std::string::npos);
        CHECK(result.find("IN[0].gl_Position") == std::string::npos);
    }

    // --- Pattern 5: IN[0].v_xxx → gs_in_v_xxx[0] ---

    TEST_CASE("IN[0].v_Color → gs_in_v_Color[0]") {
        std::string in     = "vec4 c = IN[0].v_Color;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("gs_in_v_Color[0]") != std::string::npos);
        CHECK(result.find("IN[0].v_Color") == std::string::npos);
    }

    TEST_CASE("IN[0].v_TexCoord → gs_in_v_TexCoord[0]") {
        std::string in     = "vec2 uv = IN[0].v_TexCoord.xy;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("gs_in_v_TexCoord[0].xy") != std::string::npos);
    }

    // --- Pattern 6: PS_INPUT variable declaration removed ---

    TEST_CASE("PS_INPUT v; removed") {
        std::string in     = "PS_INPUT v;\nvoid main() {}";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("PS_INPUT v;") == std::string::npos);
    }

    // --- Pattern 6a: PS_INPUT return type → void ---

    TEST_CASE("PS_INPUT as function return type → void") {
        std::string in     = "PS_INPUT makeVertex() { }";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("void makeVertex()") != std::string::npos);
    }

    // --- Pattern 6b: VS_OUTPUT parameter removal ---

    TEST_CASE("trailing VS_OUTPUT parameter removed") {
        std::string in     = "void emit(inout TriangleStream OUT, in VS_OUTPUT IN) {}";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("VS_OUTPUT") == std::string::npos);
        CHECK(result.find("inout TriangleStream OUT") != std::string::npos);
    }

    TEST_CASE("leading VS_OUTPUT parameter removed") {
        std::string in     = "void func(in VS_OUTPUT IN, inout TriangleStream OUT) {}";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("VS_OUTPUT") == std::string::npos);
    }

    // --- Pattern 6c: IN.gl_Position → gl_in[0].gl_Position ---

    TEST_CASE("IN.gl_Position → gl_in[0].gl_Position") {
        std::string in     = "vec4 p = IN.gl_Position;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("gl_in[0].gl_Position") != std::string::npos);
    }

    // --- Pattern 6d: IN.v_xxx → gs_in_v_xxx[0] ---

    TEST_CASE("IN.v_Color → gs_in_v_Color[0]") {
        std::string in     = "vec4 c = IN.v_Color;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("gs_in_v_Color[0]") != std::string::npos);
    }

    // --- Pattern 6e: bare IN[0] removal from function args ---

    TEST_CASE("trailing IN[0] removed from args") {
        std::string in     = "func(OUT, IN[0]);";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("func(OUT)") != std::string::npos);
    }

    TEST_CASE("leading IN[0] removed from args") {
        std::string in     = "func(IN[0], OUT);";
        auto        result = TranslateGeometryShader(in);
        bool        found  = result.find("func( OUT)") != std::string::npos ||
                     result.find("func(OUT)") != std::string::npos;
        CHECK(found);
    }

    // --- Pattern 6f: return v; → return; ---

    TEST_CASE("return v; → return;") {
        std::string in     = "return v;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("return;") != std::string::npos);
        CHECK(result.find("return v;") == std::string::npos);
    }

    TEST_CASE("return value; not affected") {
        std::string in     = "return value;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("return value;") != std::string::npos);
    }

    // --- Pattern 7: v.gl_Position → gl_Position ---

    TEST_CASE("v.gl_Position → gl_Position") {
        std::string in     = "v.gl_Position = vec4(1.0);";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("gl_Position = vec4(1.0)") != std::string::npos);
        CHECK(result.find("v.gl_Position") == std::string::npos);
    }

    // --- Pattern 8: v.v_xxx → v_xxx ---

    TEST_CASE("v.v_Color → v_Color") {
        std::string in     = "v.v_Color = vec4(1.0);";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("v_Color = vec4(1.0)") != std::string::npos);
        CHECK(result.find("v.v_Color") == std::string::npos);
    }

    TEST_CASE("v.v_TexCoord.xy preserved swizzle") {
        std::string in     = "v.v_TexCoord.xy = uv;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("v_TexCoord.xy = uv") != std::string::npos);
    }

    // --- Pattern 8.5: vec3 varying = mul() → .xyz ---

    TEST_CASE("v_WorldPos = mul() gets .xyz") {
        std::string in     = "v_WorldPos = mul(pos, mat);";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("v_WorldPos = (mul(pos, mat)).xyz;") != std::string::npos);
    }

    TEST_CASE("v_ScreenCoord = mul() gets .xyz") {
        std::string in     = "v_ScreenCoord = mul(pos, mat);";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find(".xyz;") != std::string::npos);
    }

    TEST_CASE("non-vec3 varying = mul() unchanged") {
        std::string in     = "v_Other = mul(pos, mat);";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find(".xyz") == std::string::npos);
    }

    // --- Pattern 9: OUT.Append() ---

    TEST_CASE("OUT.Append(v) → EmitVertex()") {
        std::string in     = "OUT.Append(v);";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("EmitVertex();") != std::string::npos);
        CHECK(result.find("OUT.Append") == std::string::npos);
    }

    TEST_CASE("OUT.Append(FuncCall()) → FuncCall(); EmitVertex()") {
        std::string in     = "OUT.Append(makeVert(a, b));";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("makeVert(a, b); EmitVertex();") != std::string::npos);
    }

    TEST_CASE("OUT.Append with nested parens") {
        std::string in     = "OUT.Append(func(a, vec2(1.0, 2.0)));";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("EmitVertex()") != std::string::npos);
        CHECK(result.find("func(a, vec2(1.0, 2.0))") != std::string::npos);
    }

    TEST_CASE("OUT.Append with space before semicolon") {
        std::string in     = "OUT.Append(makeVert()) ;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("makeVert(); EmitVertex();") != std::string::npos);
        CHECK(result.find("OUT.Append") == std::string::npos);
    }

    TEST_CASE("OUT.Append preserves code before and after") {
        std::string in     = "float x = 1.0;\nOUT.Append(buildVert(a, b));\nfloat y = 2.0;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("float x = 1.0") != std::string::npos);
        CHECK(result.find("float y = 2.0") != std::string::npos);
        CHECK(result.find("buildVert(a, b); EmitVertex();") != std::string::npos);
    }

    TEST_CASE("multiple OUT.Append in same shader") {
        std::string in     = "OUT.Append(makeA());\nOUT.Append(makeB());";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("makeA(); EmitVertex();") != std::string::npos);
        CHECK(result.find("makeB(); EmitVertex();") != std::string::npos);
    }

    // --- Pattern 10: OUT.RestartStrip() → EndPrimitive() ---

    TEST_CASE("OUT.RestartStrip() → EndPrimitive()") {
        std::string in     = "OUT.RestartStrip();";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("EndPrimitive();") != std::string::npos);
        CHECK(result.find("OUT.RestartStrip") == std::string::npos);
    }

    // --- Pattern 11: for-loop variable shadowing ---

    TEST_CASE("for loop int s shadowing float s renamed") {
        std::string in     = "for(int s=0;s<N;++s){float s=smoothstep(0.0,1.0,t);}";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("_si") != std::string::npos);
        CHECK(result.find("float s") != std::string::npos);
    }

    TEST_CASE("for loop int s without float s unchanged") {
        std::string in     = "for(int s=0;s<N;++s){float t=1.0;}";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("_si") == std::string::npos);
    }

    // --- Pattern 12: gl_in[0].gl_Position vec4→vec3 truncation for function args ---

    TEST_CASE("gl_in[0].gl_Position truncated to .xyz for vec3 param") {
        std::string in     = "vec3 doSomething(vec3 pos) { return pos; }\nvoid main() { "
                             "doSomething(gl_in[0].gl_Position); }";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("gl_in[0].gl_Position.xyz") != std::string::npos);
    }

    TEST_CASE("gl_in[0].gl_Position NOT truncated for vec4 param") {
        std::string in =
            "void doSomething(vec4 pos) { }\nvoid main() { doSomething(gl_in[0].gl_Position); }";
        auto result = TranslateGeometryShader(in);
        // Should NOT have .xyz after gl_Position
        auto pos = result.find("gl_in[0].gl_Position");
        CHECK(pos != std::string::npos);
        // Check the char after "gl_in[0].gl_Position" is not '.'
        size_t after = pos + std::string("gl_in[0].gl_Position").size();
        if (after < result.size()) {
            CHECK(result[after] != '.');
        }
    }

    TEST_CASE("gl_in[0].gl_Position as second arg gets .xyz") {
        std::string in     = "vec3 doSomething(float f, vec3 pos) { return pos; }\nvoid main() { "
                             "doSomething(1.0, gl_in[0].gl_Position); }";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("gl_in[0].gl_Position.xyz") != std::string::npos);
    }

    TEST_CASE("gl_in[0].gl_Position in nested call correctly scanned") {
        std::string in =
            "vec3 outer(vec3 p) { return p; }\nvoid main() { outer(gl_in[0].gl_Position); }";
        auto result = TranslateGeometryShader(in);
        CHECK(result.find("gl_in[0].gl_Position.xyz") != std::string::npos);
    }

    TEST_CASE("multiple gl_in[0].gl_Position occurrences all processed") {
        std::string in     = "vec3 f(vec3 p) { return p; }\nvoid main() { f(gl_in[0].gl_Position); "
                             "f(gl_in[0].gl_Position); }";
        auto        result = TranslateGeometryShader(in);
        // Count occurrences of .xyz
        size_t count = 0;
        size_t pos   = 0;
        while ((pos = result.find("gl_in[0].gl_Position.xyz", pos)) != std::string::npos) {
            count++;
            pos += 24;
        }
        CHECK(count == 2);
    }

    TEST_CASE("gl_in[0].gl_Position with existing swizzle not touched") {
        std::string in     = "vec3 doSomething(vec3 pos) { return pos; }\nvoid main() { "
                             "doSomething(gl_in[0].gl_Position.xyz); }";
        auto        result = TranslateGeometryShader(in);
        // Should not double-swizzle
        CHECK(result.find("gl_in[0].gl_Position.xyz.xyz") == std::string::npos);
        CHECK(result.find("gl_in[0].gl_Position.xyz") != std::string::npos);
    }

    // --- Combined test: full geometry shader translation ---

    TEST_CASE("combined geometry shader translation") {
        std::string in     = "[maxvertexcount(4)]\n"
                             "in vec4 gl_Position;\n"
                             "out vec4 gl_Position;\n"
                             "in vec4 v_Color;\n"
                             "in vec2 v_TexCoord;\n"
                             "void main(in VS_OUTPUT IN, inout TriangleStream OUT) {\n"
                             "  PS_INPUT v;\n"
                             "  v.gl_Position = IN[0].gl_Position;\n"
                             "  v.v_Color = IN[0].v_Color;\n"
                             "  OUT.Append(v);\n"
                             "  OUT.RestartStrip();\n"
                             "}\n";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("#define WE_GS_MAX_VERTICES (4)") != std::string::npos);
        CHECK(result.find("gs_in_v_Color[]") != std::string::npos);
        CHECK(result.find("gs_in_v_TexCoord[]") != std::string::npos);
        CHECK(result.find("gl_in[0].gl_Position") != std::string::npos);
        CHECK(result.find("gs_in_v_Color[0]") != std::string::npos);
        CHECK(result.find("EmitVertex()") != std::string::npos);
        CHECK(result.find("EndPrimitive()") != std::string::npos);
        CHECK(result.find("PS_INPUT") == std::string::npos);
        CHECK(result.find("VS_OUTPUT") == std::string::npos);
    }

    TEST_CASE("multiple OUT.Append preserves inter-call code exactly") {
        // Two Append calls with code between them. The second call's prefix extraction
        // depends on pos being updated correctly after the first call.
        // If start-pos mutated to start+pos, the second call's prefix would be wrong.
        std::string in     = "int a=1; OUT.Append(f1()); int b=2; OUT.Append(f2()); int c=3;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("int a=1; f1(); EmitVertex(); int b=2; f2(); EmitVertex(); int c=3;") !=
              std::string::npos);
    }

    TEST_CASE("OUT.Append exact extraction: no extra chars leaked") {
        // Tests that start-pos subtraction and i-1-inner are exact
        std::string in     = "AAA OUT.Append(compute(x, y)); BBB";
        auto        result = TranslateGeometryShader(in);
        // Result should be: "AAA compute(x, y); EmitVertex(); BBB"
        CHECK(result.find("AAA compute(x, y); EmitVertex(); BBB") != std::string::npos);
    }

    TEST_CASE("OUT.Append with deeply nested parens") {
        std::string in     = "OUT.Append(a(b(c(d))));";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("a(b(c(d))); EmitVertex();") != std::string::npos);
    }

    TEST_CASE("OUT.Append without semicolon") {
        std::string in     = "OUT.Append(makeVert())\nnext line";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("makeVert(); EmitVertex()") != std::string::npos);
        CHECK(result.find("next line") != std::string::npos);
    }

    TEST_CASE("OUT.Append with tab before semicolon") {
        std::string in     = "OUT.Append(v)\t;";
        auto        result = TranslateGeometryShader(in);
        CHECK(result.find("EmitVertex();") != std::string::npos);
    }

    TEST_CASE("gl_in[0].gl_Position with existing swizzle not truncated") {
        std::string in =
            "vec3 f(vec3 p) { return p; }\nvoid main() { f(gl_in[0].gl_Position.xyz); }";
        auto result = TranslateGeometryShader(in);
        // Should keep .xyz, NOT add another .xyz
        auto pos = result.find("gl_in[0].gl_Position.xyz");
        CHECK(pos != std::string::npos);
        // No double .xyz.xyz
        CHECK(result.find("gl_in[0].gl_Position.xyz.xyz") == std::string::npos);
    }

    TEST_CASE("gl_in[0].gl_Position multiple occurrences") {
        std::string in     = "vec3 f(vec3 a, vec3 b) { return a; }\n"
                             "void main() { f(gl_in[0].gl_Position, gl_in[0].gl_Position); }";
        auto        result = TranslateGeometryShader(in);
        // Both should get .xyz
        size_t first  = result.find("gl_in[0].gl_Position.xyz");
        size_t second = result.find("gl_in[0].gl_Position.xyz", first + 1);
        CHECK(first != std::string::npos);
        CHECK(second != std::string::npos);
    }

} // TEST_SUITE("TranslateGeometryShader")

// ===========================================================================
// StripStrayEndifs
// ===========================================================================

TEST_SUITE("FixFragmentGlPosition") {
    TEST_CASE("gl_Position in fragment shader → gl_FragCoord") {
        std::string in  = "float rnd = noise(gl_Position.xy);";
        std::string out = "float rnd = noise(gl_FragCoord.xy);";
        CHECK_EQ(FixFragmentGlPosition(in), out);
    }

    TEST_CASE("multiple gl_Position references all replaced") {
        std::string in  = "vec2 a = gl_Position.xy; float b = gl_Position.z;";
        std::string out = "vec2 a = gl_FragCoord.xy; float b = gl_FragCoord.z;";
        CHECK_EQ(FixFragmentGlPosition(in), out);
    }

    TEST_CASE("no gl_Position → unchanged") {
        std::string in = "vec4 color = texture(tex, uv);";
        CHECK_EQ(FixFragmentGlPosition(in), in);
    }

    TEST_CASE("identifier containing gl_Position not touched") {
        std::string in = "float mygl_PositionHack = 1.0;";
        CHECK_EQ(FixFragmentGlPosition(in), in);
    }

    TEST_CASE("gl_PositionIn not touched (different identifier)") {
        std::string in = "vec4 a = gl_PositionIn[0];";
        CHECK_EQ(FixFragmentGlPosition(in), in);
    }

} // TEST_SUITE("FixFragmentGlPosition")

TEST_SUITE("StripStrayEndifs") {
    TEST_CASE("no preprocessor directives — unchanged") {
        std::string in = "void main() {\n  gl_Position = vec4(0);\n}\n";
        CHECK_EQ(StripStrayEndifs(in), in);
    }

    TEST_CASE("balanced #if/#endif — unchanged") {
        std::string in = "#if FOO\nint x = 1;\n#endif\nvoid main() {}\n";
        CHECK_EQ(StripStrayEndifs(in), in);
    }

    TEST_CASE("stray #endif removed") {
        std::string in  = "#if FOO\nint x;\n#endif\n#endif\nvoid main() {}\n";
        std::string out = "#if FOO\nint x;\n#endif\nvoid main() {}\n";
        CHECK_EQ(StripStrayEndifs(in), out);
    }

    TEST_CASE("multiple stray #endifs removed") {
        std::string in  = "#endif\n#if A\n#endif\n#endif\n#endif\ncode;\n";
        std::string out = "#if A\n#endif\ncode;\n";
        CHECK_EQ(StripStrayEndifs(in), out);
    }

    TEST_CASE("stray #else removed — code between kept") {
        std::string in     = "#if X\nfoo;\n#endif\n#else\nbar;\n#endif\n";
        auto        result = StripStrayEndifs(in);
        // Stray #else and #endif stripped, but bar; (regular code) survives
        CHECK(result.find("#else") == std::string::npos);
        CHECK(result.find("bar;") != std::string::npos);
        CHECK(result.find("foo;") != std::string::npos);
    }

    TEST_CASE("nested #if/#else/#endif preserved") {
        std::string in = "#if A\n#if B\nx;\n#else\ny;\n#endif\n#endif\n";
        CHECK_EQ(StripStrayEndifs(in), in);
    }

    TEST_CASE("indented directives handled") {
        std::string in  = "  #if FOO\n  code;\n  #endif\n  #endif\n";
        std::string out = "  #if FOO\n  code;\n  #endif\n";
        CHECK_EQ(StripStrayEndifs(in), out);
    }

    TEST_CASE("Simple_Audio_Bars pattern — stray #endif mid-main") {
        std::string in = "void main() {\n"
                         "#if BAR_STYLE == 1\n"
                         "  setup();\n"
                         "#endif\n"
                         "#if DEFORMITY == 3\n"
                         "  deform();\n"
                         "#endif\n"
                         "#endif\n" // stray
                         "#if TRANSFORM\n"
                         "  transform();\n"
                         "#endif\n"
                         "  gl_Position = pos;\n"
                         "}\n";
        auto result = StripStrayEndifs(in);
        CHECK(result.find("gl_Position") != std::string::npos);
        CHECK(result.find("}\n") != std::string::npos);
        // The stray #endif should be gone, but the rest preserved
        // Count #if and #endif — should be balanced
        int         ifs = 0, endifs = 0;
        std::size_t p = 0;
        while ((p = result.find("#if", p)) != std::string::npos) {
            ++ifs;
            p += 3;
        }
        p = 0;
        while ((p = result.find("#endif", p)) != std::string::npos) {
            ++endifs;
            p += 6;
        }
        CHECK_EQ(ifs, endifs);
    }

} // TEST_SUITE("StripStrayEndifs")

// --- ClampAudioReactiveShift ---

TEST_SUITE("ClampAudioReactiveShift") {
    TEST_CASE("clamps u_rOffset * v_AudioShift inside texture sample") {
        std::string in =
            "vec4 rValue = texture(g_Texture0, v_TexCoord.xy - (u_rOffset * v_AudioShift));";
        auto result = ClampAudioReactiveShift(in);
        CHECK(result.find("u_rOffset * min(v_AudioShift, 1.0)") != std::string::npos);
        CHECK(result.find("u_rOffset * v_AudioShift)") == std::string::npos);
    }

    TEST_CASE("clamps all three u_rOffset/u_gOffset/u_bOffset occurrences") {
        std::string in     = "r = tex(t, c - (u_rOffset * v_AudioShift));\n"
                             "g = tex(t, c - (u_gOffset * v_AudioShift));\n"
                             "b = tex(t, c - (u_bOffset * v_AudioShift));\n";
        auto        result = ClampAudioReactiveShift(in);
        CHECK(result.find("u_rOffset * min(v_AudioShift, 1.0)") != std::string::npos);
        CHECK(result.find("u_gOffset * min(v_AudioShift, 1.0)") != std::string::npos);
        CHECK(result.find("u_bOffset * min(v_AudioShift, 1.0)") != std::string::npos);
    }

    TEST_CASE("shader without v_AudioShift unchanged") {
        std::string in     = "vec4 c = texture(g_Tex, v_TexCoord);\n";
        auto        result = ClampAudioReactiveShift(in);
        CHECK_EQ(result, in);
    }

    TEST_CASE("v_AudioShift without u_*Offset multiplier unchanged") {
        // Other uses of v_AudioShift (e.g. as a scale parameter) are not
        // touched — the clamp is scoped specifically to UV sampling offsets.
        std::string in     = "float scale = 1.0 + v_AudioShift * 0.2;\n";
        auto        result = ClampAudioReactiveShift(in);
        CHECK_EQ(result, in);
    }

    TEST_CASE("whitespace variations") {
        std::string in = "r = tex(t, c - (u_rOffset*v_AudioShift));\n"    // no spaces
                         "g = tex(t, c - (u_gOffset  *  v_AudioShift));"; // extra spaces
        auto result = ClampAudioReactiveShift(in);
        CHECK(result.find("u_rOffset * min(v_AudioShift, 1.0)") != std::string::npos);
        CHECK(result.find("u_gOffset * min(v_AudioShift, 1.0)") != std::string::npos);
    }

} // TEST_SUITE("ClampAudioReactiveShift")

// ===========================================================================
// FixImplicitConversions — HLSL→GLSL implicit-truncation patterns surfaced
// by the 2026-04-29 library audit (Naruto family + Outset Island).
// ===========================================================================

TEST_SUITE("FixImplicitConversions.libraryAudit") {

    TEST_CASE("float = vec2_expr wraps RHS with .x") {
        // Lens Flare Sun (workshop 2487531853, 5 wallpapers):
        // `float pointer = g_PointerPosition.xy * u_pointerSpeed;`
        std::string in =
            "uniform vec2 g_PointerPosition;\n"
            "uniform float u_pointerSpeed;\n"
            "void main() {\n"
            "  float pointer = g_PointerPosition.xy * u_pointerSpeed;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("(g_PointerPosition.xy * u_pointerSpeed).x") !=
              std::string::npos);
    }

    TEST_CASE("float = scalar function call NOT wrapped (Cyberpunk Lucy regression)") {
        // 2866203962 color_key effect:
        // `float delta = _wedot(abs(g_KeyColor - albedo.rgb), vec3(1, 1, 1));`
        // _wedot returns float — wrapping with .x errors as scalar swizzle.
        std::string in =
            "void main() {\n"
            "  vec3 g_KeyColor;\n"
            "  vec4 albedo;\n"
            "  float delta = _wedot(abs(g_KeyColor - albedo.rgb), vec3(1, 1, 1));\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        // No `.x` appended after the closing paren of _wedot.
        CHECK(result.find("_wedot(abs(g_KeyColor - albedo.rgb), vec3(1, 1, 1)).x") ==
              std::string::npos);
    }

    TEST_CASE("varying shadowed by local float skips wider-varying truncation") {
        // Lens Flare Sun: `varying vec4 timer; float timer = sin(...);`
        // `rotation + timer * timer2` should NOT get `.xy` swizzles —
        // bare `timer` resolves to the float local.
        std::string in =
            "in vec4 timer;\n"
            "in vec4 timer2;\n"
            "in vec2 rotation;\n"
            "void main() {\n"
            "  float timer = 1.0;\n"
            "  float timer2 = 2.0;\n"
            "  vec2 result = rotation + timer * timer2;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        // No `timer.xy` / `timer.xyz` should have been injected.
        CHECK(result.find("timer.xy") == std::string::npos);
        CHECK(result.find("timer.xyz") == std::string::npos);
    }

    TEST_CASE("vec3 varying minus vec2() constructor gets .xy swizzle") {
        // Cutout Vignette (workshop 2138904733, Outset Island):
        // `length(abs(v_TexCoord - vec2(u_offset)) * 1.0)` where
        // v_TexCoord is `in vec3`.  Expect `v_TexCoord.xy - vec2(...)`.
        std::string in =
            "in vec3 v_TexCoord;\n"
            "uniform vec2 u_offset;\n"
            "void main() {\n"
            "  float scale = length(abs(v_TexCoord - vec2(u_offset)) * 1.0);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("v_TexCoord.xy - vec2(u_offset)") != std::string::npos);
    }

    TEST_CASE("scalar swizzle on shadowed varying rewrites to vecN()") {
        // If `float NAME = ...` shadows `varying vec4 NAME` and someone
        // writes `NAME.xy`, GLSL rejects the scalar swizzle.  Rewrite to
        // `vec2(NAME)` so HLSL's broadcast semantics keep working.
        std::string in =
            "in vec4 timer;\n"
            "void main() {\n"
            "  float timer = 1.0;\n"
            "  vec2 broadcast = timer.xy;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("vec2(timer)") != std::string::npos);
        // The bare `timer.xy` should have been replaced.
        CHECK(result.find("= timer.xy") == std::string::npos);
    }

} // TEST_SUITE("FixImplicitConversions.libraryAudit")

// ===========================================================================
// Preamble extensions — regression for 2026-05-15 audit log spam
// ===========================================================================
TEST_SUITE("FixImplicitConversions.scalarBroadcast") {
    // Pattern from Girl Error System Arona (3341577331) / Daisies (3501635854)
    // bokeh / chromatic-aberration shaders.
    TEST_CASE("vec2 VAR = scalar literal broadcasts to vec2") {
        std::string in =
            "void main() {\n"
            "  vec2 radial = 0.0;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("vec2 radial = vec2(0.0);") != std::string::npos);
    }

    TEST_CASE("vec3 / vec4 broadcast from scalar literal") {
        std::string in =
            "void main() {\n"
            "  vec3 a = 1.0;\n"
            "  vec4 b = 0.5;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("vec3 a = vec3(1.0)") != std::string::npos);
        CHECK(result.find("vec4 b = vec4(0.5)") != std::string::npos);
    }

    TEST_CASE("mixed comma list: scalar + vector inits in one declaration") {
        // The Arona / Daisies bokeh shader writes
        //   `vec2 radial = 0.0, tangential = 0.0, center = (1.0 - u_center - 0.5) * u_general;`
        // Where u_center is `uniform vec2` and u_general is `uniform float`.
        // The scalar literals must broadcast; the third initialiser already
        // produces a vec2 (vec2 - vec2 - vec2) and must NOT be touched.
        std::string in =
            "uniform vec2 u_center;\n"
            "uniform float u_general;\n"
            "void main() {\n"
            "  vec2 radial = 0.0, tangential = 0.0, center = (1.0 - u_center - 0.5) * u_general;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("radial = vec2(0.0)") != std::string::npos);
        CHECK(result.find("tangential = vec2(0.0)") != std::string::npos);
        // The vector initialiser must be preserved verbatim (no double-wrap).
        CHECK(result.find("vec2((1.0 - u_center") == std::string::npos);
        CHECK(result.find("center = (1.0 - u_center - 0.5) * u_general") != std::string::npos);
    }

    TEST_CASE("does not double-wrap an already-vec initialiser") {
        std::string in =
            "void main() {\n"
            "  vec2 a = vec2(0.5);\n"
            "  vec3 b = vec3(1.0, 0.0, 0.0);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        // No vec2(vec2(...)) anywhere.
        CHECK(result.find("vec2(vec2(") == std::string::npos);
        CHECK(result.find("vec3(vec3(") == std::string::npos);
    }

    TEST_CASE("texture() RHS is recognised as vector") {
        std::string in =
            "uniform sampler2D s;\n"
            "void main() {\n"
            "  vec4 c = texture(s, vec2(0.0, 0.0));\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        // Texture call must not be wrapped.
        CHECK(result.find("vec4(texture(") == std::string::npos);
    }

    TEST_CASE("function parameter list is not mistaken for a decl with init") {
        // `vec2 foo(int x)` has no `=` in its body, so the regex shouldn't
        // even consider it.  Verifies non-init declarations are untouched.
        std::string in = "vec2 foo(int x) { return vec2(0); }\n";
        auto result = FixImplicitConversions(in);
        CHECK(result == in);
    }

    TEST_CASE("vec2 VAR = vec4_varying narrows to .xy") {
        // Pattern from Arona's chromatic_aberration: `vec2 _m_v_TexCoord = v_TexCoord;`
        std::string in =
            "in vec4 v_TexCoord;\n"
            "void main() {\n"
            "  vec2 _m_v_TexCoord = v_TexCoord;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("vec2 _m_v_TexCoord = v_TexCoord.xy;") != std::string::npos);
    }

    TEST_CASE("vec3 VAR = vec4_uniform narrows to .xyz") {
        std::string in =
            "uniform vec4 u_color;\n"
            "void main() {\n"
            "  vec3 col = u_color;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("vec3 col = u_color.xyz;") != std::string::npos);
    }

    TEST_CASE("vec2 VAR = vec3_varying narrows to .xy") {
        std::string in =
            "in vec3 v_PointerUV;\n"
            "void main() {\n"
            "  vec2 da = v_PointerUV;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("vec2 da = v_PointerUV.xy;") != std::string::npos);
    }

    TEST_CASE("identical-width assignment is left untouched") {
        std::string in =
            "in vec4 v_TexCoord;\n"
            "void main() {\n"
            "  vec4 copy = v_TexCoord;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        // No swizzle added.
        CHECK(result.find("v_TexCoord.") == std::string::npos);
    }

    TEST_CASE("RHS with member access is left untouched") {
        // Already explicit narrowing — don't double up.
        std::string in =
            "in vec4 v_TexCoord;\n"
            "void main() {\n"
            "  vec2 t = v_TexCoord.xy;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("v_TexCoord.xy.xy") == std::string::npos);
    }

    TEST_CASE("vec2 + vec3-swizzle is fixed (either truncated or broadcast)") {
        // Workshop effect 2892816289 (Daisies / Bush / Chill / Into-the-
        // Starfield share it) ships `tetraNoise(p + e.xyy)` where p is vec2
        // and e.xyy is vec3.  Existing fixArithSwizzleTrunc truncates
        // `e.xyy` → `e.xy` (since tetraNoise actually takes vec2 here).  When
        // the function expects vec3 instead, my new broadcast wraps p in
        // `vec3(p, 0.0)`.  Either resolves the vec2+vec3 mismatch.
        std::string in =
            "void main() {\n"
            "  vec2 p = vec2(0.0);\n"
            "  vec2 e = vec2(0.0);\n"
            "  float r = tetraNoise(p + e.xyy);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        bool ok = result.find("e.xy)") != std::string::npos ||
                  result.find("vec3(p, 0.0) + e.xyy") != std::string::npos;
        CHECK(ok);
    }

    TEST_CASE("shadowed-name vec2+3-swizzle is handled by scope-aware broadcast") {
        // Daisies (3501635854) and Bush family share workshop effect
        // 2892816289 which declares `vec3 p` inside hsv2rgb AND `vec2 p` in
        // main() — both scopes coexist in the dumped shader.
        //
        // fixArithSwizzleTrunc skips shadowed names (otherwise truncating
        // `e.xyy` to `e.xy` in main scope would be re-expanded by
        // fixArithSwizzleExpand when it iterates vec3-named `p`).  The
        // scope-aware broadcast pass walks backward from each `p OP 3swiz`
        // match to find the nearest preceding `vec[234] p` declaration; only
        // rewrites when that nearest decl is vec2.  Output in main scope:
        // `vec3(p, 0.0) + e.xyy` (valid vec3+vec3).  hsv2rgb's `p - K.xx`
        // gets a 3-swizzle expansion via fixArithSwizzleExpand → `p - K.xxx`.
        std::string in =
            "vec3 hsv2rgb(vec3 c) {\n"
            "  vec3 p = c.xyz;\n"
            "  return p;\n"
            "}\n"
            "void main() {\n"
            "  vec2 p = vec2(0.0);\n"
            "  vec2 e = vec2(0.0);\n"
            "  float r = tetraNoise(p + e.xyy);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        auto pos = result.find("void main()");
        REQUIRE(pos != std::string::npos);
        std::string main_body = result.substr(pos);
        // main scope: `p` wrapped as vec3
        CHECK(main_body.find("vec3(p, 0.0) + e.xyy") != std::string::npos);
        // hsv2rgb scope: `p` left alone (it's vec3 there)
        std::string hsv_body = result.substr(0, pos);
        CHECK(hsv_body.find("vec3(p, 0.0)") == std::string::npos);
    }

    TEST_CASE("vec2 + 3-swizzle with vec3-shadow: only main-scope vec2 gets wrapped") {
        // `v` declared as vec3 in myFunc AND vec2 in main.  Scope-aware
        // broadcast wraps `v` in main but not in myFunc.
        std::string in =
            "vec3 myFunc(vec3 v) {\n"
            "  vec3 v = vec3(0.0);\n"
            "  return v;\n"
            "}\n"
            "void main() {\n"
            "  vec2 v = vec2(0.0);\n"
            "  vec2 e = vec2(0.0);\n"
            "  float r = tetraNoise(v + e.xyy);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        auto pos = result.find("void main()");
        REQUIRE(pos != std::string::npos);
        // Wrap appears only in main body (vec2 scope)
        std::string main_body = result.substr(pos);
        std::string func_body = result.substr(0, pos);
        CHECK(main_body.find("vec3(v, 0.0)") != std::string::npos);
        CHECK(func_body.find("vec3(v, 0.0)") == std::string::npos);
    }

    TEST_CASE("global const vecN init referencing uniform drops const qualifier") {
        // Jett × Jinx (3031735486) ships
        //   const vec2 type = vec2(g_Texture0Resolution.x / g_Texture0Resolution.y, 1.0)
        //                     * vec2(g_ratio, 1.0);
        // — both factors involve uniforms.  Glslang errors: "global const
        // initializers must be constant 2-component vector of float".
        std::string in =
            "uniform vec4 g_Texture0Resolution;\n"
            "uniform float g_ratio;\n"
            "const vec2 type = vec2(g_Texture0Resolution.x, 1.0) * vec2(g_ratio, 1.0);\n";
        auto result = FixImplicitConversions(in);
        // const qualifier dropped:
        CHECK(result.find("const vec2 type") == std::string::npos);
        CHECK(result.find("vec2 type = vec2(") != std::string::npos);
    }

    TEST_CASE("global const vecN with literal initializer keeps const") {
        // The strip is only when the RHS references a uniform.  Pure literal
        // initializers stay const.
        std::string in = "const vec3 K = vec3(0.299, 0.587, 0.114);\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("const vec3 K = vec3(0.299, 0.587, 0.114);") != std::string::npos);
    }

    TEST_CASE("vec2 = (_wedot(...) * scalar) broadcasts even with nested vec2() args") {
        // Workshop noise function commonly written as:
        //   float rand_1_05(in vec2 uv) {
        //       vec2 noise = (fract(sin(_wedot(uv.yx, vec2(12.9898, 78.233) * 2.0)) * 43758.5453));
        //       return abs(noise.x + noise.y) * 10. * -1.;
        //   }
        // The `_wedot(...)` (our `dot` override) ALWAYS returns float, so the
        // outer chain (`fract(sin(scalar) * scalar)`) is scalar.  Pre-fix the
        // is_scalar_expr heuristic bailed on ANY `vec2(` anywhere.  Now it
        // walks at depth 0 and treats `_wedot/dot/length/distance` calls as
        // scalar producers, ignoring vec[234]() that appears nested as their
        // arguments.  Hits Moon (2157202681), Bocchi PA-san (2896906752),
        // Bocchi すっからかん (2899677910), and others.
        std::string in =
            "void main() {\n"
            "  vec2 uv = vec2(0.0);\n"
            "  vec2 noise = (fract(sin(_wedot(uv.yx, vec2(12.9898, 78.233) * 2.0)) * 43758.5453));\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("vec2 noise = vec2(") != std::string::npos);
    }

    TEST_CASE("for-loop bare float uniform init/cond gets int() wrapped") {
        // Workshop audio-spectrum effects (3034862641 / 3036962127 / 3496072356):
        //   uniform float u_MinFreqRange;
        //   uniform float u_MaxFreqRange;
        //   for (int i = u_MinFreqRange; i < u_MaxFreqRange; i++) { … }
        // HLSL silently casts float→int in the loop bounds; GLSL rejects.
        std::string in =
            "uniform float u_MinFreqRange;\n"
            "uniform float u_MaxFreqRange;\n"
            "void main() {\n"
            "  float left = 0.0;\n"
            "  for (int i = u_MinFreqRange; i < u_MaxFreqRange; i++) {\n"
            "    left += 1.0;\n"
            "  }\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("for (int i = int(u_MinFreqRange)") != std::string::npos);
        CHECK(result.find("; i < int(u_MaxFreqRange);") != std::string::npos);
    }

    TEST_CASE("for-loop float-uniform wrap does not double-wrap or touch non-uniform") {
        // Don't wrap a local float (only matches `uniform float`), and don't
        // re-wrap if already in int(...).
        std::string in =
            "uniform float u_lim;\n"
            "void main() {\n"
            "  float local = 0.0;\n"
            "  for (int i = local; i < int(u_lim); i++) {\n"
            "    local += 1.0;\n"
            "  }\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        // local stays alone (no int() wrap — it's not a uniform).
        CHECK(result.find("for (int i = local;") != std::string::npos);
        // u_lim already int-wrapped — must not nest.
        CHECK(result.find("int(int(u_lim))") == std::string::npos);
    }

    TEST_CASE("vec2 LHS truncates wider varying on the LHS of an arithmetic op") {
        // Arona (3341577331) chromatic_aberration vertex shader:
        //   out vec4 v_PointerUV;
        //   vec2 da = v_PointerUV * (g_Scale * g_Scale_FollowCursor_Multiplier) * 0.001;
        // Existing wider-varying pass only catches bare adjacency
        // (`wname OP nn` or `wname OP vecN(...)`) — parenthesized
        // expressions like `(g_Scale * …)` weren't matched.  The new
        // LHS-rank-aware pass detects the `vec2 da = …` declaration and
        // injects `.xy` after the wider name when adjacent to an
        // arithmetic operator.
        std::string in =
            "uniform vec2 g_Scale;\n"
            "out vec4 v_PointerUV;\n"
            "void main() {\n"
            "  vec2 da = v_PointerUV * (g_Scale * 2.0) * 0.001;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("v_PointerUV.xy *") != std::string::npos);
    }

    TEST_CASE("vec2 LHS leaves wider varying alone when passed bare to function") {
        // Same contract as the existing wider-varying tests: bare argument
        // to a function call shouldn't get .xy because the callee may
        // legitimately accept the wider type.
        std::string in =
            "in vec4 v_TexCoord;\n"
            "void main() {\n"
            "  vec2 r = someFunc(v_TexCoord);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("v_TexCoord.xy") == std::string::npos);
        CHECK(result.find("someFunc(v_TexCoord)") != std::string::npos);
    }

    TEST_CASE("vec2 LHS skips shadowed varying") {
        // `varying vec4 timer; float timer = …;` — bare `timer` in vec2 LHS
        // RHS resolves to the float local, not the varying.  Must not add .xy.
        std::string in =
            "in vec4 timer;\n"
            "in vec2 rotation;\n"
            "void main() {\n"
            "  float timer = 1.0;\n"
            "  vec2 r = rotation + timer * 2.0;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("timer.xy") == std::string::npos);
    }

    TEST_CASE("mix(named_vec, scalar.x, mask) broadcasts the dot-access") {
        // 2k+ Retro Cyber Sunset (1117550117) / Perfect View (1235913324) /
        // Moon (2157202681) all share workshop effect 2079712247
        // reflection.frag which writes
        //   glOutColor = vec4(mix(mix(albedo, reflected.x, mask), …))
        // where `albedo` and `reflected` are local vec4.  glslang rejected
        // `mix(vec4, float, float)` with "no matching overloaded function".
        // The transform should detect arg0 is a known vec4 and arg1 is a
        // single-component swizzle on a known vec, then broadcast arg1.
        std::string in =
            "uniform float mask;\n"
            "void main() {\n"
            "  vec4 albedo = vec4(1.0);\n"
            "  vec4 reflected = vec4(0.5);\n"
            "  vec4 c = mix(albedo, reflected.x, mask);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("mix(albedo, vec4(reflected.x)") != std::string::npos);
    }

    TEST_CASE("mix(named_vec, float_ident, float_ident) broadcasts arg1") {
        std::string in =
            "uniform float gray;\n"
            "uniform float t;\n"
            "void main() {\n"
            "  vec3 col = vec3(0.0);\n"
            "  col = mix(col, gray, t);\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("mix(col, vec3(gray),") != std::string::npos);
    }

    TEST_CASE("paren-cmp-arith does not glue float onto preceding keyword") {
        // Mikey Tokyo Revengers (2622312893) dot_matrix shader:
        //   float in01(float f) { return (f >=0) * (f < 1); }
        // glslang's preprocessor dropped the space between `return` and the
        // open-paren, leaving `return(f >= 0) * ...`.  Pre-fix, the cmp→float
        // transform turned `(f >= 0) *` into `float(f >= 0) *` without
        // accounting for the preceding token, emitting `returnfloat(f >= 0)`
        // — an undefined identifier glslang flagged with
        //   'returnfloat' : no matching overloaded function found.
        std::string in =
            "float in01(float f) {\n"
            "  return(f >= 0) * (f < 1);\n"   // simulate preprocessor-glued case
            "}\n";
        auto result = FixImplicitConversions(in);
        // Must keep `return` separate from `float`.
        CHECK(result.find("returnfloat") == std::string::npos);
        CHECK(result.find("return float(f >= 0)") != std::string::npos);
    }

    TEST_CASE("paren-cmp-arith still wraps the standalone case") {
        std::string in =
            "void main() {\n"
            "  float x = (a > 0) * b;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("float(a > 0) * b") != std::string::npos);
    }

    TEST_CASE("out vec4 varying multiplied by narrower vec gets .xy/.xyz") {
        // Arona (3341577331) vertex shader: `out vec4 v_PointerUV;` then
        // `vec2 da = v_PointerUV * (g_Scale * g_Scale_FollowCursor_Multiplier) * 0.001;`
        // — needs `.xy` injected because g_Scale is uniform vec2.
        std::string in =
            "uniform vec2 g_Scale;\n"
            "out vec4 v_PointerUV;\n"
            "void main() {\n"
            "  vec2 da = v_PointerUV * g_Scale * 0.001;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("v_PointerUV.xy * g_Scale") != std::string::npos);
    }

    TEST_CASE("function body's first `= scalar; ` is not absorbed into the return type") {
        // Regression: the original `vec[234] (\w+\s*=[^;]+);` guard caught
        // function declarations like `vec3 NAME(args) { ... ; const float
        // AvgLumR = 0.5; }` — the regex spanned from `vec3` (the return type)
        // to the first `;` inside the body, and is_scalar_expr saw a scalar
        // RHS, so we emitted `vec3 NAME(args) { ... ; const float AvgLumR =
        // vec3(0.5); }`.  Found in Daisies' ContrastSaturationBrightness.
        std::string in =
            "vec3 ContrastSaturationBrightness(vec3 color, float brt, float sat, float con)\n"
            "{\n"
            "    const float AvgLumR = 0.5;\n"
            "    return color;\n"
            "}\n";
        auto result = FixImplicitConversions(in);
        CHECK(result.find("const float AvgLumR = 0.5;") != std::string::npos);
        // Must NOT have broadcast the float scalar to vec3.
        CHECK(result.find("AvgLumR = vec3(0.5)") == std::string::npos);
    }
}

TEST_SUITE("WPShaderPreamble.extensions") {
    using namespace wallpaper;

    TEST_CASE("common preamble enables GL_EXT_spec_constant_composites") {
        // Many WE shaders declare const-initialised vec4 tables at file scope
        // (colour palettes, coefficient tables).  glslang classifies those as
        // candidate spec-constant aggregates and warns until the extension is
        // enabled.  Without enable, every wallpaper using such tables spammed
        // 60+ warnings into the audit log (Daisies 3501635854 alone hit ~50).
        const std::string preamble = kPreShaderCodeCommon;
        CHECK(preamble.find("#extension GL_EXT_spec_constant_composites : enable") !=
              std::string::npos);
    }

    TEST_CASE("preamble keeps #version 330 ahead of #extension") {
        // GLSL parser rejects #extension above #version, so order matters.
        const std::string preamble = kPreShaderCodeCommon;
        auto              vpos     = preamble.find("#version 330");
        auto              epos     = preamble.find("#extension GL_EXT_spec_constant_composites");
        REQUIRE(vpos != std::string::npos);
        REQUIRE(epos != std::string::npos);
        CHECK(vpos < epos);
    }

    TEST_CASE("preamble retains the __SHADER_PLACEHOLD__ marker") {
        // WPShaderParser appends combo #defines after the placeholder line;
        // dropping it would break the preprocessor chain.
        const std::string preamble = kPreShaderCodeCommon;
        CHECK(preamble.find("__SHADER_PLACEHOLD__") != std::string::npos);
    }
}

TEST_SUITE("FixImplicitConversions.intPromotion") {
    TEST_CASE("recurses into nested builtin args (Cybering 2326102392)") {
        // Cybering ships:
        //   step(0.99, _wedot(step(0, uv) * step(uv, 1), (vec2(0.5))))
        // Outer `step(0.99,…)` parses cleanly, but the inner `step(0, uv)`
        // and `step(uv, 1)` use bare integer literals.  GLSL rejects them.
        // The int-promotion pass must descend INTO the outer call to fix them.
        std::string in =
            "float f(vec2 uv){\n"
            " return step(0.99, _wedot(step(0, uv) * step(uv, 1), vec2(0.5)));\n"
            "}";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("step(0,") == std::string::npos);
        CHECK(out.find(", 1)") == std::string::npos);
        CHECK(out.find("step(0.0,") != std::string::npos);
        CHECK(out.find(", 1.0)") != std::string::npos);
    }

    TEST_CASE("first arg of step (no preceding separator)") {
        // The first argument has no preceding `,` — verify the up-front
        // check promotes it.  Regression for the leading-arg blind spot.
        std::string in  = "void g(vec2 uv){ float a = step(2, uv); }";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("step(2,") == std::string::npos);
        CHECK(out.find("step(2.0,") != std::string::npos);
    }

    TEST_CASE("does NOT corrupt floats with exponent") {
        // 1e3 is already a float — must not become 1.0e3.
        std::string in  = "void h(vec2 x){ float a = step(1e3, x); }";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("step(1.0e3") == std::string::npos);
        CHECK(out.find("step(1e3") != std::string::npos);
    }

    TEST_CASE("does NOT corrupt negative-exponent literals (The Blur)") {
        // The Blur (3562086244) ships `_wemx(1e-6, u_NotchSize * ...)`.
        // The `-` in `1e-6` must not be treated as a separator that triggers
        // int promotion of `6` → `6.0` (which would yield `1e-6.0`).
        std::string in  = "void k(){ float a = _wemx(1e-6, b); }";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("1e-6.0") == std::string::npos);
        CHECK(out.find("1e-6") != std::string::npos);
    }
}

TEST_SUITE("FixImplicitConversions.assorted") {
    TEST_CASE("brace-list vec4 ctor (Now Playing) gets braces stripped") {
        // HLSL allows `vec4({1,2,3,4})`; GLSL rejects with "unexpected
        // LEFT_BRACE".  Driver: Now Playing (2883312700).
        std::string in =
            "void m(){ vec4 albedo = vec4({ 1.0, 1.0, 1.0, 0.0 }); }";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("vec4({") == std::string::npos);
        CHECK(out.find("vec4( 1.0, 1.0, 1.0, 0.0 )") != std::string::npos);
    }

    TEST_CASE("bool * float (Mikey) wraps comparison with float()") {
        // HLSL implicit bool→float; GLSL has no `float * bool`.
        // Driver: Mikey Tokyo Revengers (2622312893).
        std::string in =
            "float in01(float f){ return float(f >= 0) * (f < 1); }";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("* (f < 1)") == std::string::npos);
        CHECK(out.find("* float(f < 1)") != std::string::npos);
    }

    TEST_CASE("float = step(scalar, vec4_var) appends .x (cyberpunk)") {
        // Driver: cyberpunk edgerunners (2885492021):
        //   float r = step(1.0, albedo);   // albedo declared vec4 above
        // step(float, vec4) returns vec4 — GLSL refuses to init float.
        std::string in =
            "void m(){ vec4 albedo = vec4(0.0); float r = step(1.0, albedo); }";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("step(1.0, albedo);") == std::string::npos);
        CHECK(out.find("step(1.0, albedo).x;") != std::string::npos);
    }

    TEST_CASE("float = step(scalar, vec_var.swizzle) leaves call alone") {
        // The .z extracts a scalar — call returns float — no narrowing.
        // Driver: 发光少女 (3287715210) — `_westep(0.0, v_TexCoordFx.z)`.
        std::string in =
            "in vec4 v_TexCoordFx;\n"
            "void m(){ float mask = step(0.0, v_TexCoordFx.z); }";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("step(0.0, v_TexCoordFx.z);") != std::string::npos);
        CHECK(out.find(".z).x;") == std::string::npos);
    }

    TEST_CASE("const drop: uniform initializer (发光少女)") {
        std::string in =
            "uniform float u_hueShift;\n"
            "void m(){ const float cos_a = cos(radians(u_hueShift)); }";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("const float cos_a") == std::string::npos);
        CHECK(out.find("float cos_a = cos") != std::string::npos);
    }

    TEST_CASE("const drop: member access (cyberpunk)") {
        std::string in =
            "void m(){ vec4 albedo = vec4(0.0);\n"
            " const vec4 __vec4 = vec4(albedo.rgb, 0.0); }";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("const vec4 __vec4") == std::string::npos);
        CHECK(out.find("vec4 __vec4 = vec4(albedo.rgb, 0.0);") != std::string::npos);
    }

    TEST_CASE("const drop: keeps const on literal-only initializer") {
        // `const vec3 k = vec3(0.57735);` is compile-time constant — keep it.
        std::string in =
            "void m(){ const vec3 k = vec3(0.57735); }";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("const vec3 k") != std::string::npos);
    }
}

TEST_SUITE("WPShaderParser.crossStageVaryingNarrowing") {
    // Cross-stage varying upgrade widens fragment's `in vec2 v_TexCoord;`
    // to `in vec4` to match the vertex stage's wider output.  After this
    // upgrade, bare `vec2 X = v_TexCoord;` becomes a type mismatch.  The
    // upgrade pass must also rewrite those initializers.
    //
    // We can't easily test the full cross-stage pass here because it
    // requires multi-unit input.  This is covered end-to-end by
    // Fami/Adventurous/rhythm-in-garden audits.  Placeholder marker so we
    // know to refresh the audit if the upgrade logic changes.
    TEST_CASE("marker: see wp-audit for Fami/Adventurous/rhythm in garden") {
        CHECK(true);
    }
}

TEST_SUITE("FixImplicitConversions.smoothstep_narrow") {
    TEST_CASE("smoothstep(vec, vec OP scalar, scalar) → narrow to .x (Mikey)") {
        // Mikey Tokyo Revengers (2622312893):
        //   vec2 dist = vec2(length(p));
        //   fragLV += smoothstep(dist, dist + smoothing, thresh);
        // Without narrowing, glslang reports "no matching overloaded
        // smoothstep" because no overload accepts (vec2, vec2, float).
        std::string in =
            "void m(){\n"
            " vec2 dist = vec2(1.0);\n"
            " float smoothing = 0.5;\n"
            " float thresh = 0.5;\n"
            " float v = smoothstep(dist, dist + smoothing, thresh);\n"
            "}";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("smoothstep(dist.x, dist.x + smoothing, thresh)") != std::string::npos);
    }

    TEST_CASE("smoothstep with non-vec first arg unchanged") {
        std::string in =
            "void m(){\n"
            " float a = 0.5;\n"
            " float v = smoothstep(a, a + 0.1, 0.7);\n"
            "}";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("smoothstep(a, a + 0.1, 0.7)") != std::string::npos);
    }
}

TEST_SUITE("FixImplicitConversions.intToFloat") {
    TEST_CASE("int = _westep(...) → float = _westep(...) (Chill Time)") {
        // Chill Time (2925278995):
        //   int bar = _westep(1 - shapeCoord.y, barHeight);
        // _westep returns float; GLSL refuses float→int implicit narrowing.
        std::string in =
            "void m(vec2 shapeCoord){\n"
            " float barHeight = 0.5;\n"
            " int bar = _westep(1.0 - shapeCoord.y, barHeight);\n"
            "}";
        std::string out = FixImplicitConversions(in);
        CHECK(out.find("int bar = _westep") == std::string::npos);
        CHECK(out.find("float bar = _westep") != std::string::npos);
    }
}

// ===========================================================================
// FixImplicitConversions — composite golden characterization (T0 safety net)
// ===========================================================================
// These pin the WHOLE-pipeline output of FixImplicitConversions for
// representative HLSL→GLSL inputs.  Unlike the substring assertions above, each
// CHECK_EQ freezes the EXACT composite result of all ~45 ordered phases.  They
// exist so that mechanical refactors of FixImplicitConversions (static-regex
// hoist, lambda/phase extraction) are provably non-regressing: any byte of
// drift in the combined output fails here.  The expected strings were captured
// from the current implementation; do NOT hand-edit them — if a CHECK_EQ fails
// after a behaviour-preserving refactor, the refactor changed output (a bug),
// not the golden.  Each golden targets a distinct phase cluster the existing
// substring cases exercise.
TEST_SUITE("FixImplicitConversions.goldenCharacterization") {
    // Cluster: direct vec→vec truncation on assignment (vec2=vec4, vec3=vec4,
    // vec2=vec3 each gain the right swizzle).
    TEST_CASE("golden: direct vec truncation") {
        std::string in =
            "void main() {\n"
            "  vec4 b;\n"
            "  vec3 c;\n"
            "  vec2 a;\n"
            "  a = b;\n"
            "  c = b;\n"
            "  a = c;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "void main() {\n"
                 "  vec4 b;\n"
                 "  vec3 c;\n"
                 "  vec2 a;\n"
                 "  a = b.xy;\n"
                 "  c = b.xyz;\n"
                 "  a = c.xy;\n"
                 "}\n");
    }

    // Cluster: arithmetic swizzle truncation (mix-rank in `+`) — wider operand
    // narrowed to the narrower LHS width on both operand orders.
    TEST_CASE("golden: arithmetic swizzle truncation") {
        std::string in =
            "void main() {\n"
            "  vec2 a = vec2(0);\n"
            "  vec4 b;\n"
            "  vec3 d;\n"
            "  vec2 r = a + b.xyzw;\n"
            "  vec2 s = d.xyz + a;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "void main() {\n"
                 "  vec2 a = vec2(0);\n"
                 "  vec4 b;\n"
                 "  vec3 d;\n"
                 "  vec2 r = a + b.xy;\n"
                 "  vec2 s = d.xy + a;\n"
                 "}\n");
    }

    // Cluster: uint modulo (three sub-patterns) + general `word % N`.
    TEST_CASE("golden: uint modulo + general modulo") {
        std::string in =
            "void main() {\n"
            "  uint b = (a + 1) % 32;\n"
            "  uint c = (a - 3) % 16;\n"
            "  uint idx = val % 8;\n"
            "  x = foo % 10;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "void main() {\n"
                 "  uint b = uint((int(a) + 1) % 32);\n"
                 "  uint c = uint((int(a) - 3) % 16);\n"
                 "  uint idx = uint(int(val) % 8);\n"
                 "  x = int(foo) % 10;\n"
                 "}\n");
    }

    // Cluster: ternary condition is a bare integer literal → wrapped with bool().
    TEST_CASE("golden: integer ternary condition wrapped with bool") {
        std::string in =
            "void main() {\n"
            "  float r = (1 ? a : b);\n"
            "  float s = cond , 0 ? c : d;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "void main() {\n"
                 "  float r = (bool(1) ? a : b);\n"
                 "  float s = cond , bool(0) ? c : d;\n"
                 "}\n");
    }

    // Cluster: int-promotion of bare integer literals inside (nested) builtin
    // call args — the pass descends into the outer call (Cybering 2326102392).
    TEST_CASE("golden: int promotion in nested builtin args") {
        std::string in =
            "float f(vec2 uv){\n"
            "  return step(0.99, _wedot(step(0, uv) * step(uv, 1), vec2(0.5)));\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "float f(vec2 uv){\n"
                 "  return step(0.99, _wedot(step(0.0, uv) * step(uv, 1.0), vec2(0.5)));\n"
                 "}\n");
    }

    // Cluster: scalar broadcast — `float = vecN_expr` gains a trailing `.x`
    // (Lens Flare Sun 2487531853).
    TEST_CASE("golden: scalar broadcast appends .x") {
        std::string in =
            "uniform vec2 g_PointerPosition;\n"
            "uniform float u_pointerSpeed;\n"
            "void main() {\n"
            "  float pointer = g_PointerPosition.xy * u_pointerSpeed;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "uniform vec2 g_PointerPosition;\n"
                 "uniform float u_pointerSpeed;\n"
                 "void main() {\n"
                 "  float pointer = (g_PointerPosition.xy * u_pointerSpeed).x;\n"
                 "}\n");
    }

    // Cluster: for-loop int initializer/condition with a uniform float bound
    // get wrapped in int(...).
    TEST_CASE("golden: for-loop int init/cond promotion") {
        std::string in =
            "uniform float g_Count;\n"
            "void main() {\n"
            "  for (int i = -g_Count * 2; i <= g_Count * 2; ++i) {\n"
            "    accum += i;\n"
            "  }\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "uniform float g_Count;\n"
                 "void main() {\n"
                 "  for (int i = int(-g_Count * 2); i <= int(g_Count) * 2; ++i) {\n"
                 "    accum += i;\n"
                 "  }\n"
                 "}\n");
    }

    // Cluster: `float VAR = int(...)` becomes `int VAR`; `const` dropped on a
    // non-constant initializer but KEPT on a literal-only one.
    TEST_CASE("golden: float-int ctor swap + const drop heuristic") {
        std::string in =
            "uniform float u_hueShift;\n"
            "void main() {\n"
            "  float x = int(foo);\n"
            "  const float cos_a = cos(radians(u_hueShift));\n"
            "  const vec3 k = vec3(0.57735);\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "uniform float u_hueShift;\n"
                 "void main() {\n"
                 "  int x = int(foo);\n"
                 "  float cos_a = cos(radians(u_hueShift));\n"
                 "  const vec3 k = vec3(0.57735);\n"
                 "}\n");
    }

    // Cluster: mix(scalar, vecN(scalar), ...) rank handling + bool*float wrap
    // (Mikey 2622312893).
    TEST_CASE("golden: mix rank + bool multiply wrap") {
        std::string in =
            "float in01(float f){ return float(f >= 0) * (f < 1); }\n"
            "void main(){\n"
            "  float m = mix(a, vec4(b), t);\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "float in01(float f){ return float(f >= 0) * float(f < 1); }\n"
                 "void main(){\n"
                 "  float m = mix(a, vec4(b), t);\n"
                 "}\n");
    }

    // Cluster: brace-list vec ctor stripped (Now Playing 2883312700) +
    // smoothstep(vec, vec OP scalar, scalar) narrowed to .x (Mikey).
    TEST_CASE("golden: brace-list ctor strip + smoothstep narrow") {
        std::string in =
            "void main(){\n"
            "  vec4 albedo = vec4({ 1.0, 1.0, 1.0, 0.0 });\n"
            "  vec2 dist = vec2(1.0);\n"
            "  float smoothing = 0.5;\n"
            "  float thresh = 0.5;\n"
            "  float v = smoothstep(dist, dist + smoothing, thresh);\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "void main(){\n"
                 "  vec4 albedo = vec4( 1.0, 1.0, 1.0, 0.0 );\n"
                 "  vec2 dist = vec2(1.0);\n"
                 "  float smoothing = 0.5;\n"
                 "  float thresh = 0.5;\n"
                 "  float v = smoothstep(dist.x, dist.x + smoothing, thresh);\n"
                 "}\n");
    }

    // Cluster: fixTrunc with multiple (dst,src) pairs — all four assignments in one
    // result string must be truncated sequentially.  Validates the multi-pair sequential
    // scan the B1 one-pass rewrite must preserve.
    TEST_CASE("golden: multi-pair direct truncation — four assignments all converted") {
        std::string in =
            "void main() {\n"
            "  vec4 c;\n"
            "  vec3 b;\n"
            "  vec2 a;\n"
            "  vec2 d;\n"
            "  a = b;\n"
            "  a = c;\n"
            "  d = b;\n"
            "  d = c;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        CHECK_EQ(out,
                 "void main() {\n"
                 "  vec4 c;\n"
                 "  vec3 b;\n"
                 "  vec2 a;\n"
                 "  vec2 d;\n"
                 "  a = b.xy;\n"
                 "  a = c.xy;\n"
                 "  d = b.xy;\n"
                 "  d = c.xy;\n"
                 "}\n");
    }

    // Cluster: fixTrunc shadowing guard — when a name appears in both the dst set and
    // the src set (vec2 and vec4 declarations of the same identifier), the assignment
    // must NOT be truncated.  The vec2-typed local is skipped; only the unambiguous
    // vec2 `a` gets the .xy swizzle.
    TEST_CASE("golden: fixTrunc shadowing guard — same name in dst and src left untouched") {
        // `cursor` is declared as both vec2 (function param) and vec4 (local in main).
        // fixTrunc must not truncate `cursor = ...` because `cursor` is in both vec2_vars
        // and vec4_vars.  Only `a = cursor` (unambiguous vec2 dst, vec4 src) is rewritten.
        std::string in =
            "vec2 calcStroke(vec2 cursor) {\n"
            "  return cursor;\n"
            "}\n"
            "void main() {\n"
            "  vec4 cursor = u_mousePos;\n"
            "  vec2 a;\n"
            "  cursor = cursor;\n"
            "  a = cursor;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        // `cursor = cursor;` — dst cursor is in vec2_vars AND vec4_vars, so the guard fires
        // and it is left unchanged.  `a = cursor;` — a is vec2 only, cursor is vec4, so it
        // becomes `a = cursor.xy;`.
        CHECK_EQ(out,
                 "vec2 calcStroke(vec2 cursor) {\n"
                 "  return cursor;\n"
                 "}\n"
                 "void main() {\n"
                 "  vec4 cursor = u_mousePos;\n"
                 "  vec2 a;\n"
                 "  cursor = cursor;\n"
                 "  a = cursor.xy;\n"
                 "}\n");
    }

    // Cluster: same name `p` declared as vec3 in hsv2rgb and vec2 in main.
    // fixArithSwizzleTrunc skips `p` for both trunc_vec2 and trunc_vec3 because it's
    // in the shadow set.  fixArithSwizzleExpand fires for `p` in hsv2rgb (vec3 p OP .xx
    // → .xxx).  main's `p + e.xyy` is handled by the broadcast pass (further down in the
    // function), not by Trunc.
    TEST_CASE("golden: shadowed name p — vec3/vec2 scoping via Expand, not Trunc") {
        std::string in =
            "vec3 hsv2rgb(vec3 p) {\n"
            "  vec3 K = vec3(1.0);\n"
            "  return p - K.xx;\n"
            "}\n"
            "void main() {\n"
            "  vec2 p = vec2(0.5, 0.5);\n"
            "  vec3 e = vec3(0.1);\n"
            "  vec3 col = hsv2rgb(vec3(p, 0.0) + e.xyy);\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        // Both `p` names (vec3 param + vec2 local) land in the shadow set, so fixArithSwizzleTrunc
        // and fixArithSwizzleExpand skip the `p` operand entirely.  K.xx is also left alone because
        // the Expand pass finds `p` shadowed and does not fire for this expression.  The broadcast
        // pass in main (vec3(p, 0.0) + e.xyy) does not match the Expand pattern either.
        // Output is the input unchanged.
        CHECK_EQ(out,
                 "vec3 hsv2rgb(vec3 p) {\n"
                 "  vec3 K = vec3(1.0);\n"
                 "  return p - K.xx;\n"
                 "}\n"
                 "void main() {\n"
                 "  vec2 p = vec2(0.5, 0.5);\n"
                 "  vec3 e = vec3(0.1);\n"
                 "  vec3 col = hsv2rgb(vec3(p, 0.0) + e.xyy);\n"
                 "}\n");
    }

    // Cluster: upgradeIfOutOfRange for vec2 and vec3 paths — both declarations upgraded,
    // both texture() calls gain their swizzle.
    TEST_CASE("golden: vec2-to-vec4 and vec3-to-vec4 upgrade with texture swizzle") {
        std::string in =
            "in vec2 v_TexCoord;\n"
            "in vec3 v_Normal;\n"
            "uniform sampler2D s_tex;\n"
            "void main() {\n"
            "  float z = v_TexCoord.z;\n"
            "  float w = v_Normal.w;\n"
            "  vec4 c0 = texture(s_tex, v_TexCoord);\n"
            "  vec4 c1 = texture(s_tex, v_Normal);\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        // v_TexCoord: .z access → upgraded to vec4; texture() gains .xy
        // v_Normal: .w access → upgraded to vec4; texture() gains .xyz
        CHECK_EQ(out,
                 "in vec4 v_TexCoord;\n"
                 "in vec4 v_Normal;\n"
                 "uniform sampler2D s_tex;\n"
                 "void main() {\n"
                 "  float z = v_TexCoord.z;\n"
                 "  float w = v_Normal.w;\n"
                 "  vec4 c0 = texture(s_tex, v_TexCoord.xy);\n"
                 "  vec4 c1 = texture(s_tex, v_Normal.xyz);\n"
                 "}\n");
    }

    // Cluster: tex-scalar block — vec4 var assigned from texture() whose bare name
    // appears in `float * vec4_var` arithmetic; bare use sites get .x appended but
    // the declaration and any `in vec4` form are preserved.
    TEST_CASE("golden: tex-scalar — vec4 texture var in float arithmetic gets .x at use site") {
        std::string in =
            "uniform float u_scale;\n"
            "uniform sampler2D s_tex;\n"
            "void main() {\n"
            "  vec2 uv = v_TexCoord.xy;\n"
            "  vec4 timer = texture(s_tex, uv);\n"
            "  float off = u_scale * timer;\n"
            "  float result = timer * u_scale + 1.0;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        // `timer` is used in `u_scale * timer` — used_as_scalar = true.
        // bare `timer` → `timer.x` at use sites; declaration preserved.
        CHECK_EQ(out,
                 "uniform float u_scale;\n"
                 "uniform sampler2D s_tex;\n"
                 "void main() {\n"
                 "  vec2 uv = v_TexCoord.xy;\n"
                 "  vec4 timer = texture(s_tex, uv);\n"
                 "  float off = u_scale * timer.x;\n"
                 "  float result = timer.x * u_scale + 1.0;\n"
                 "}\n");
    }

    // Cluster: fixArithSwizzleTrunc reverse pattern — WORD.xyzw OP vec2_var;
    // the swizzle is on the left and the named variable is on the right.  Swizzle
    // truncated from 4 to 2.
    TEST_CASE("golden: arithmetic swizzle truncation — reverse operand order") {
        std::string in =
            "void main() {\n"
            "  vec2 a = vec2(0.0);\n"
            "  vec4 b;\n"
            "  vec2 r = b.xyzw + a;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        // b.xyzw is on the left; a (vec2 local) is on the right.
        // Reverse pattern fires: b.xyzw → b.xy.
        CHECK_EQ(out,
                 "void main() {\n"
                 "  vec2 a = vec2(0.0);\n"
                 "  vec4 b;\n"
                 "  vec2 r = b.xy + a;\n"
                 "}\n");
    }

    // Cluster: fixArithSwizzleExpand skip-guard — when the matched variable position is
    // immediately followed by a `.` in the original text, the guard returns the original
    // match unchanged (prevents double-expansion).
    TEST_CASE("golden: fixArithSwizzleExpand skips variable that already has a swizzle") {
        // foo.rgb is a vec3 variable accessed with .rgb; the textRef[vEnd]=='.' guard fires
        // because the matched `foo` is followed by a dot.  The expression is left unchanged.
        // Note: this requires `foo` to be in local_vec3 (i.e. `vec3 foo = ...`).
        std::string in =
            "void main() {\n"
            "  vec3 foo = vec3(1.0);\n"
            "  vec3 K = vec3(0.5);\n"
            "  vec3 r = foo.rgb - K.xx;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        // `foo.rgb - K.xx`: the Expand pass matches `foo` as the left operand but
        // textRef[vEnd] == '.' fires the skip-guard, so `foo.rgb` is left unchanged.
        // `K` is the right operand; K.xx stays as-is because `K` is in local_vec3 but
        // the Expand pass only fires when the LEFT operand is the named variable —
        // K is the right operand here, so no expansion.  Output is unchanged.
        CHECK_EQ(out,
                 "void main() {\n"
                 "  vec3 foo = vec3(1.0);\n"
                 "  vec3 K = vec3(0.5);\n"
                 "  vec3 r = foo.rgb - K.xx;\n"
                 "}\n");
    }

    // Cluster: upgradeIfOutOfRange with two independent variables — both upgraded,
    // no cross-contamination between their texture() swizzles.
    TEST_CASE("golden: two independent vec2-to-vec4 upgrades no cross-contamination") {
        std::string in =
            "in vec2 tc0;\n"
            "in vec2 tc1;\n"
            "uniform sampler2D s0;\n"
            "uniform sampler2D s1;\n"
            "void main() {\n"
            "  float z0 = tc0.z;\n"
            "  float z1 = tc1.z;\n"
            "  vec4 c0 = texture(s0, tc0);\n"
            "  vec4 c1 = texture(s1, tc1);\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        // Both tc0 and tc1: .z access → upgraded to vec4; each texture() call gains .xy
        CHECK_EQ(out,
                 "in vec4 tc0;\n"
                 "in vec4 tc1;\n"
                 "uniform sampler2D s0;\n"
                 "uniform sampler2D s1;\n"
                 "void main() {\n"
                 "  float z0 = tc0.z;\n"
                 "  float z1 = tc1.z;\n"
                 "  vec4 c0 = texture(s0, tc0.xy);\n"
                 "  vec4 c1 = texture(s1, tc1.xy);\n"
                 "}\n");
    }

    // Cluster: fixArithSwizzleTrunc forward-only path — named vec2 local is the LEFT
    // operand; the wider swizzle (.xyzw, 4 components) is on the RIGHT.  Only the
    // forward regex fires (VAR OP WORD.XXXX); no reverse pattern is present here.
    TEST_CASE("golden: arithmetic swizzle truncation — forward operand order") {
        std::string in =
            "void main() {\n"
            "  vec2 uv = vec2(0.5);\n"
            "  vec4 col;\n"
            "  vec2 r = uv * col.xyzw;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        // uv (vec2 local, LEFT operand) OP col.xyzw (4-component swizzle, RIGHT) →
        // forward Trunc fires: col.xyzw truncated to col.xy.
        CHECK_EQ(out,
                 "void main() {\n"
                 "  vec2 uv = vec2(0.5);\n"
                 "  vec4 col;\n"
                 "  vec2 r = uv * col.xy;\n"
                 "}\n");
    }

    // Cluster: fixArithSwizzleExpand positive path — unshadowed vec3 local is the LEFT
    // operand in arithmetic with a 2-component swizzle on the RIGHT; skip-guard does NOT
    // fire because the variable has no trailing dot.  Swizzle expanded by repeating last char.
    TEST_CASE("golden: arithmetic swizzle expansion — vec3 var widens narrow swizzle") {
        std::string in =
            "void main() {\n"
            "  vec3 light = vec3(1.0);\n"
            "  vec4 ambient;\n"
            "  vec3 result = light + ambient.xy;\n"
            "}\n";
        std::string out = FixImplicitConversions(in);
        // light (vec3 local, LEFT, no trailing dot — skip-guard does NOT fire) OP ambient.xy
        // (2-component swizzle, RIGHT) → Expand fires: ambient.xy padded to ambient.xyy.
        CHECK_EQ(out,
                 "void main() {\n"
                 "  vec3 light = vec3(1.0);\n"
                 "  vec4 ambient;\n"
                 "  vec3 result = light + ambient.xyy;\n"
                 "}\n");
    }
}

TEST_SUITE("VolumetricPreamble") {
    using namespace wallpaper;

    TEST_CASE("preamble defines sampler2DBackBuffer alias") {
        const std::string preamble = kPreShaderCodeCommon;
        // Aliased to plain sampler2D — WE-side helper depends on resolution-aware
        // sampling, which our renderer collapses to a direct texture() lookup.
        CHECK(preamble.find("#define sampler2DBackBuffer sampler2D") !=
              std::string::npos);
    }

    TEST_CASE("sampler2DBackBuffer alias keeps decl well formed after concat") {
        const std::string preamble = kPreShaderCodeCommon;
        const std::string shader =
            "uniform sampler2DBackBuffer g_Texture1;\n";
        const std::string combined = preamble + shader;
        CHECK(combined.find("#define sampler2DBackBuffer sampler2D") !=
              std::string::npos);
        CHECK(combined.find("uniform sampler2DBackBuffer g_Texture1;") !=
              std::string::npos);
    }

    TEST_CASE("preamble keeps existing helpers intact after insertion") {
        const std::string preamble = kPreShaderCodeCommon;
        CHECK(preamble.find("#define texSample2D texture")  != std::string::npos);
        CHECK(preamble.find("#define mul(x, y) ((y) * (x))") != std::string::npos);
        CHECK(preamble.find("#define frac fract")            != std::string::npos);
        CHECK(preamble.find("#define lerp mix")              != std::string::npos);
        CHECK(preamble.find("#define saturate(x)")           != std::string::npos);
        CHECK(preamble.find("#define step _westep")          != std::string::npos);
    }

    TEST_CASE("preamble defines sampler2DComparison alias") {
        const std::string preamble = kPreShaderCodeCommon;
        // SHADOW=0 path means the binding is never reached; aliasing to plain
        // sampler2D keeps the never-emitted decl well-formed for glslang.  When
        // shadow-mapped volumetrics ships, this alias retypes to sampler2DShadow.
        CHECK(preamble.find("#define sampler2DComparison sampler2D") !=
              std::string::npos);
    }

    TEST_CASE("preamble defines texSample2DBackBuffer delegating to texture") {
        const std::string preamble = kPreShaderCodeCommon;
        const std::string needle = "#define texSample2DBackBuffer";
        const auto        pos    = preamble.find(needle);
        CHECK(pos != std::string::npos);
        const auto eol = preamble.find('\n', pos);
        REQUIRE(eol != std::string::npos);
        const std::string body = preamble.substr(pos, eol - pos);
        CHECK(body.find("texture(") != std::string::npos);
    }

    TEST_CASE("preamble defines texLoad2D using texelFetch ivec2") {
        const std::string preamble = kPreShaderCodeCommon;
        const std::string needle = "#define texLoad2D";
        const auto        pos    = preamble.find(needle);
        CHECK(pos != std::string::npos);
        const auto eol = preamble.find('\n', pos);
        REQUIRE(eol != std::string::npos);
        const std::string body = preamble.substr(pos, eol - pos);
        CHECK(body.find("texelFetch(") != std::string::npos);
        CHECK(body.find("ivec2(")      != std::string::npos);
    }

    TEST_CASE("preamble defines texSample2DCompare as compile-safe placeholder") {
        const std::string preamble = kPreShaderCodeCommon;
        const std::string needle = "#define texSample2DCompare";
        const auto        pos    = preamble.find(needle);
        CHECK(pos != std::string::npos);
        const auto eol = preamble.find('\n', pos);
        REQUIRE(eol != std::string::npos);
        const std::string body = preamble.substr(pos, eol - pos);
        CHECK(body.find("vec4(1.0)") != std::string::npos);
    }
}

TEST_SUITE("TranslateHlslClip") {
    TEST_CASE("simple subtraction emits if-discard") {
        std::string in  = "void main() { clip(a - b); }";
        std::string out = TranslateHlslClip(in);
        CHECK_EQ(out, "void main() { if ((a - b) < 0.0) discard; }");
    }
    TEST_CASE("negative literal arg emits if-discard") {
        std::string in  = "void main() { clip(-1.0); }";
        std::string out = TranslateHlslClip(in);
        CHECK_EQ(out, "void main() { if ((-1.0) < 0.0) discard; }");
    }
    TEST_CASE("positive literal arg still emits if-discard") {
        std::string in  = "void main() { clip(1.0); }";
        std::string out = TranslateHlslClip(in);
        CHECK_EQ(out, "void main() { if ((1.0) < 0.0) discard; }");
    }
    TEST_CASE("nested paren arg survives balanced paren matching") {
        // `dot(a, b)` contains a comma; a naive `clip\(([^)]+)\)` regex would
        // stop at that comma.  Balanced-paren matching keeps the whole arg.
        std::string in  = "void main() { clip(dot(a, b)); }";
        std::string out = TranslateHlslClip(in);
        CHECK_EQ(out, "void main() { if ((dot(a, b)) < 0.0) discard; }");
    }
    TEST_CASE("two adjacent clip statements both rewritten") {
        std::string in  = "void main() { clip(a); clip(b); }";
        std::string out = TranslateHlslClip(in);
        CHECK_EQ(out,
                 "void main() { if ((a) < 0.0) discard; if ((b) < 0.0) discard; }");
    }
    TEST_CASE("clip inside if branch keeps surrounding braces") {
        std::string in  = "if (cond) { clip(x); }";
        std::string out = TranslateHlslClip(in);
        CHECK_EQ(out, "if (cond) { if ((x) < 0.0) discard; }");
    }
    TEST_CASE("source without clip is returned unchanged") {
        std::string in  = "void main() { float x = 1.0; }";
        std::string out = TranslateHlslClip(in);
        CHECK_EQ(out, in);
    }
    TEST_CASE("substring inside identifier is not matched") {
        // `clipped`, `aclipb`, `flipclamp` contain "clip" but the \b anchor +
        // required `(` after optional whitespace ensures only true call sites
        // match.  Bare identifier declarations stay untouched.
        std::string in  = "float aclipb = 0.0; float clipped = 1.0;";
        std::string out = TranslateHlslClip(in);
        CHECK_EQ(out, in);
    }
    TEST_CASE("whitespace between name and paren is accepted") {
        // The \s* in the regex allows the syntactically-legal `clip (...)` shape;
        // trailing `;` is still consumed by skipWhitespaceAndSemicolon.
        // We paste the inner arg verbatim (including its surrounding spaces) so
        // the rewriter stays side-effect-free.
        std::string in  = "void main() { clip ( x ); }";
        std::string out = TranslateHlslClip(in);
        CHECK_EQ(out, "void main() { if (( x ) < 0.0) discard; }");
    }
}

TEST_SUITE("CompileToSpvClipWiring") {
    TEST_CASE("fragment shader source has clip rewritten before preprocess") {
        // After WPShaderParser::CompileToSpv runs its pre-preprocess pass over a
        // fragment unit, every `clip(...);` token must be gone — this is the
        // contract the pipeline integration relies on.
        std::string in  = "void main() { clip(a - b); }";
        std::string out = TranslateHlslClip(in);
        CHECK(out.find("clip(")  == std::string::npos);
        CHECK(out.find("discard") != std::string::npos);
    }
    TEST_CASE("CompileToSpv runs clip rewrite over fragment units") {
        // Drive CompileToSpv against a minimal fragment unit. No cache dir is
        // mounted, so the call takes the synchronous in-process branch.  We
        // assert by reading unit.src AFTER the call: the clip token must be
        // gone, replaced by an if-discard.
        wallpaper::WPShaderParser::InitGlslang();
        wallpaper::fs::VFS                       vfs;        // no "cache" mount
        wallpaper::WPShaderInfo                  info;
        std::vector<wallpaper::WPShaderUnit>     units {
            { wallpaper::ShaderType::FRAGMENT,
              "void main() { clip(a - b); }",
              {} }
        };
        std::vector<wallpaper::ShaderCode>       codes;
        std::vector<wallpaper::WPShaderTexInfo>  texs;
        // Return value can be false (the snippet isn't real GLSL — references
        // undefined symbols).  We only assert on the post-translation source.
        (void)wallpaper::WPShaderParser::CompileToSpv("test-clip-wiring",
                                                     units,
                                                     codes,
                                                     vfs,
                                                     &info,
                                                     texs);
        CHECK(units[0].src.find("clip(")  == std::string::npos);
        CHECK(units[0].src.find("discard") != std::string::npos);
    }
}

TEST_SUITE("QualityCombo") {
    TEST_CASE("quality tier 4 emits define 4") {
        wallpaper::Combos combos;
        combos["QUALITY"] = "4";
        std::string out = wallpaper::WPShaderParser::PreShaderHeader(
            "// shader body\n",
            combos,
            wallpaper::ShaderType::FRAGMENT);
        CHECK(out.find("#define QUALITY 4\n") != std::string::npos);
    }
    TEST_CASE("quality tier 3 emits define 3") {
        wallpaper::Combos combos;
        combos["QUALITY"] = "3";
        std::string out = wallpaper::WPShaderParser::PreShaderHeader(
            "// body\n", combos, wallpaper::ShaderType::FRAGMENT);
        CHECK(out.find("#define QUALITY 3\n") != std::string::npos);
    }
    TEST_CASE("quality tier 2 emits define 2") {
        wallpaper::Combos combos;
        combos["QUALITY"] = "2";
        std::string out = wallpaper::WPShaderParser::PreShaderHeader(
            "// body\n", combos, wallpaper::ShaderType::FRAGMENT);
        CHECK(out.find("#define QUALITY 2\n") != std::string::npos);
    }
    TEST_CASE("quality tier 1 emits define 1") {
        wallpaper::Combos combos;
        combos["QUALITY"] = "1";
        std::string out = wallpaper::WPShaderParser::PreShaderHeader(
            "// body\n", combos, wallpaper::ShaderType::FRAGMENT);
        CHECK(out.find("#define QUALITY 1\n") != std::string::npos);
    }
    TEST_CASE("kde post-processing tier maps to quality combo values") {
        // Pin the tier->value mapping the KDE post-processing wiring will
        // consume when routed through to the volumetric material.
        // Ultra=4 ⇒ 8 samples; High=3 ⇒ 5; Med=2 ⇒ 3; Low=1 ⇒ 2 (no-shadow row).
        struct Row { const char* tier; const char* value; };
        const Row rows[] = {
            {"Ultra", "4"}, {"High", "3"}, {"Med", "2"}, {"Low", "1"},
        };
        for (const auto& row : rows) {
            wallpaper::Combos combos;
            combos["QUALITY"] = row.value;
            std::string out = wallpaper::WPShaderParser::PreShaderHeader(
                "// body\n", combos, wallpaper::ShaderType::FRAGMENT);
            INFO("tier=" << row.tier);
            std::string needle = std::string("#define QUALITY ") + row.value;
            CHECK(out.find(needle) != std::string::npos);
        }
    }
}

TEST_SUITE("VolumetricGlobalCombos") {
    TEST_CASE("shadow always emits as zero in v1") {
        // v1 of the volumetric chain skips shadow-mapped scattering entirely.
        // Emitting #define SHADOW 0 keeps the never-used shadow branch
        // dead-code so glslang's preprocessor drops it cleanly.
        wallpaper::Combos combos;
        combos["SHADOW"] = "0";
        std::string out = wallpaper::WPShaderParser::PreShaderHeader(
            "// body\n", combos, wallpaper::ShaderType::FRAGMENT);
        CHECK(out.find("#define SHADOW 0\n") != std::string::npos);
    }
    TEST_CASE("reversedepth always emits as zero in v1") {
        // Our Vulkan baseline uses classic Z (VK_COMPARE_OP_LESS, clear-to-1.0).
        // REVERSEDEPTH=0 makes the regular-z branch fire in the shader,
        // matching the weSampleSceneDepthMinGather convention used by our
        // volumetric depth path.
        wallpaper::Combos combos;
        combos["REVERSEDEPTH"] = "0";
        std::string out = wallpaper::WPShaderParser::PreShaderHeader(
            "// body\n", combos, wallpaper::ShaderType::FRAGMENT);
        CHECK(out.find("#define REVERSEDEPTH 0\n") != std::string::npos);
    }
    TEST_CASE("cookie combo is omitted not defined zero") {
        // COOKIE is `#ifdef`-gated upstream, not `#if`-gated.  Defining it to 0
        // would still take the cookie branch.  Leaving the combo out of the
        // map entirely is the correct v1 cut.
        wallpaper::Combos combos;
        // intentionally do NOT set combos["COOKIE"]
        combos["SHADOW"]       = "0";
        combos["REVERSEDEPTH"] = "0";
        std::string out = wallpaper::WPShaderParser::PreShaderHeader(
            "// body\n", combos, wallpaper::ShaderType::FRAGMENT);
        CHECK(out.find("#define COOKIE") == std::string::npos);
    }
}

TEST_SUITE("VolumetricEndToEndPreprocess") {
    TEST_CASE("volumetricsback.frag translates clean") {
        const std::string src = readWeShipped("volumetricsback.frag");
        if (src.empty()) {
            MESSAGE("WE install not present; skipping end-to-end translation test");
            return;
        }
        wallpaper::Combos combos;
        combos["SHADOW"]       = "0";
        combos["REVERSEDEPTH"] = "0";
        combos["POINTLIGHT"]   = "1";
        combos["QUALITY"]      = "2";
        auto r = compileSingleShaderFromSource(
            wallpaper::ShaderType::FRAGMENT, src, combos);
        CHECK(r.post_xlat_src.find("clip(")                   == std::string::npos);
        CHECK(r.post_xlat_src.find("sampler2DBackBuffer ")    == std::string::npos);
        CHECK(r.post_xlat_src.find("sampler2DComparison ")    == std::string::npos);
        CHECK(r.post_xlat_src.find("#version")                != std::string::npos);
    }
    TEST_CASE("volumetricsback.vert translates clean") {
        const std::string src = readWeShipped("volumetricsback.vert");
        if (src.empty()) {
            MESSAGE("WE install not present; skipping");
            return;
        }
        wallpaper::Combos combos;
        combos["POINTLIGHT"]   = "1";
        combos["REVERSEDEPTH"] = "0";
        auto r = compileSingleShaderFromSource(
            wallpaper::ShaderType::VERTEX, src, combos);
        CHECK(r.post_xlat_src.find("clip(")    == std::string::npos);
        CHECK(r.post_xlat_src.find("#version") != std::string::npos);
    }
    TEST_CASE("volumetricsfront.vert translates clean") {
        const std::string src = readWeShipped("volumetricsfront.vert");
        if (src.empty()) {
            MESSAGE("WE install not present; skipping");
            return;
        }
        wallpaper::Combos combos;
        combos["POINTLIGHT"]   = "1";
        combos["REVERSEDEPTH"] = "0";
        auto r = compileSingleShaderFromSource(
            wallpaper::ShaderType::VERTEX, src, combos);
        CHECK(r.post_xlat_src.find("clip(")    == std::string::npos);
        CHECK(r.post_xlat_src.find("#version") != std::string::npos);
    }
    TEST_CASE("volumetricsfront.frag translates clean") {
        const std::string src = readWeShipped("volumetricsfront.frag");
        if (src.empty()) {
            MESSAGE("WE install not present; skipping");
            return;
        }
        wallpaper::Combos combos;
        combos["POINTLIGHT"]   = "1";
        combos["SHADOW"]       = "0";  // no shadow atlas
        combos["FULLSCREEN"]   = "0";
        combos["REVERSEDEPTH"] = "0";
        combos["QUALITY"]      = "2";  // medium default
        // intentionally do NOT set COOKIE — it is #ifdef-gated upstream
        auto r = compileSingleShaderFromSource(
            wallpaper::ShaderType::FRAGMENT, src, combos);
        // clip() statements are rewritten to if-discard
        CHECK(r.post_xlat_src.find("clip(") == std::string::npos);
        // No leftover HLSL-flavoured sampler keywords in uniform decls
        CHECK(r.post_xlat_src.find("uniform sampler2DBackBuffer") == std::string::npos);
        CHECK(r.post_xlat_src.find("uniform sampler2DComparison") == std::string::npos);
        CHECK(r.post_xlat_src.find("#version") != std::string::npos);
    }
    TEST_CASE("blur_k3.vert translates clean") {
        const std::string src = readWeShipped("blur_k3.vert");
        if (src.empty()) {
            MESSAGE("WE install not present; skipping");
            return;
        }
        wallpaper::Combos combos;
        combos["VERTICAL"] = "0";  // horizontal pass
        auto r = compileSingleShaderFromSource(
            wallpaper::ShaderType::VERTEX, src, combos);
        CHECK(r.post_xlat_src.find("#version") != std::string::npos);
    }
    TEST_CASE("blur_k3.frag translates clean") {
        const std::string src = readWeShipped("blur_k3.frag");
        if (src.empty()) {
            MESSAGE("WE install not present; skipping");
            return;
        }
        wallpaper::Combos combos;
        combos["VERTICAL"] = "1";  // vertical pass
        auto r = compileSingleShaderFromSource(
            wallpaper::ShaderType::FRAGMENT, src, combos);
        CHECK(r.post_xlat_src.find("clip(")    == std::string::npos);
        CHECK(r.post_xlat_src.find("#version") != std::string::npos);
    }
    TEST_CASE("passthrough.vert translates clean") {
        const std::string src = readWeShipped("passthrough.vert");
        if (src.empty()) {
            MESSAGE("WE install not present; skipping");
            return;
        }
        wallpaper::Combos combos;
        // no TRANSFORM combo — fullscreen-quad branch
        auto r = compileSingleShaderFromSource(
            wallpaper::ShaderType::VERTEX, src, combos);
        CHECK(r.post_xlat_src.find("#version") != std::string::npos);
    }
    TEST_CASE("passthrough.frag translates clean") {
        const std::string src = readWeShipped("passthrough.frag");
        if (src.empty()) {
            MESSAGE("WE install not present; skipping");
            return;
        }
        wallpaper::Combos combos;
        auto r = compileSingleShaderFromSource(
            wallpaper::ShaderType::FRAGMENT, src, combos);
        CHECK(r.post_xlat_src.find("clip(")    == std::string::npos);
        CHECK(r.post_xlat_src.find("#version") != std::string::npos);
    }
    TEST_CASE("volumetricsfront.frag translates at every quality tier") {
        const std::string src = readWeShipped("volumetricsfront.frag");
        if (src.empty()) {
            MESSAGE("WE install not present; skipping");
            return;
        }
        for (const char* tier : {"1", "2", "3", "4"}) {
            wallpaper::Combos combos;
            combos["POINTLIGHT"]   = "1";
            combos["SHADOW"]       = "0";
            combos["FULLSCREEN"]   = "0";
            combos["REVERSEDEPTH"] = "0";
            combos["QUALITY"]      = tier;
            auto r = compileSingleShaderFromSource(
                wallpaper::ShaderType::FRAGMENT, src, combos);
            INFO("QUALITY=" << tier);
            CHECK(r.post_xlat_src.find("clip(") == std::string::npos);
            CHECK(r.post_xlat_src.find("#version") != std::string::npos);
        }
    }
    TEST_CASE("front shader names depth-min gather helper for the depth path") {
        // The volumetric depth-RT pipeline owns the helper weSampleSceneDepthMinGather
        // and the WE-shipped front shader sources back-depth via texSample2DBackBuffer.
        // Confirm that token appears in the raw WE source — our preamble macro
        // takes care of translating it during preprocess.
        const std::string src = readWeShipped("volumetricsfront.frag");
        if (src.empty()) {
            MESSAGE("WE install not present; skipping");
            return;
        }
        CHECK(src.find("texSample2DBackBuffer") != std::string::npos);
    }
}
