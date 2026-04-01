#include <doctest.h>
#include "WPShaderTransforms.h"

// ===========================================================================
// regexTransformAll
// ===========================================================================

TEST_SUITE("regexTransformAll") {

TEST_CASE("zero matches returns 0 and text unchanged") {
    std::string text = "hello world";
    std::regex  re("MISSING");
    int count = regexTransformAll(text, re, [](const std::smatch&) { return std::string("X"); });
    CHECK(count == 0);
    CHECK(text == "hello world");
}

TEST_CASE("single match at start") {
    std::string text = "AAA BBB";
    std::regex  re("AAA");
    int count = regexTransformAll(text, re, [](const std::smatch&) { return std::string("XX"); });
    CHECK(count == 1);
    CHECK(text == "XX BBB");
}

TEST_CASE("single match at end") {
    std::string text = "AAA BBB";
    std::regex  re("BBB");
    int count = regexTransformAll(text, re, [](const std::smatch&) { return std::string("YY"); });
    CHECK(count == 1);
    CHECK(text == "AAA YY");
}

TEST_CASE("single match in middle") {
    std::string text = "aXb";
    std::regex  re("X");
    int count = regexTransformAll(text, re, [](const std::smatch&) { return std::string("---"); });
    CHECK(count == 1);
    CHECK(text == "a---b");
}

TEST_CASE("two matches preserve inter-match text exactly") {
    std::string text = "prefixAAmiddleBBsuffix";
    std::regex  re("(AA|BB)");
    int count = regexTransformAll(text, re,
        [](const std::smatch& m) { return "[" + m[0].str() + "]"; });
    CHECK(count == 2);
    CHECK(text == "prefix[AA]middle[BB]suffix");
}

TEST_CASE("three adjacent matches") {
    std::string text = "aaa";
    std::regex  re("a");
    int count = regexTransformAll(text, re, [](const std::smatch&) { return std::string("bb"); });
    CHECK(count == 3);
    CHECK(text == "bbbbbb");
}

TEST_CASE("replacer uses match groups") {
    std::string text = "x=1; y=2; z=3;";
    std::regex  re("(\\w+)=(\\d+)");
    int count = regexTransformAll(text, re,
        [](const std::smatch& m) { return m[1].str() + " is " + m[2].str(); });
    CHECK(count == 3);
    CHECK(text == "x is 1; y is 2; z is 3;");
}

TEST_CASE("match at position 0 then later match") {
    std::string text = "AB_CD_EF";
    std::regex  re("(AB|EF)");
    int count = regexTransformAll(text, re,
        [](const std::smatch& m) { return "[" + m[0].str() + "]"; });
    CHECK(count == 2);
    CHECK(text == "[AB]_CD_[EF]");
}

TEST_CASE("replacement shorter than match") {
    std::string text = "longword short longword";
    std::regex  re("longword");
    int count = regexTransformAll(text, re, [](const std::smatch&) { return std::string("s"); });
    CHECK(count == 2);
    CHECK(text == "s short s");
}

TEST_CASE("replacement longer than match") {
    std::string text = "a b a";
    std::regex  re("a");
    int count = regexTransformAll(text, re, [](const std::smatch&) { return std::string("XXXX"); });
    CHECK(count == 2);
    CHECK(text == "XXXX b XXXX");
}

} // TEST_SUITE regexTransformAll

// ===========================================================================
// NeedsFlatDecoration
// ===========================================================================

TEST_SUITE("NeedsFlatDecoration") {

TEST_CASE("int type requires flat") {
    CHECK(NeedsFlatDecoration("in int v_Id;") == true);
}

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
    std::string in  = "vec2 tc;\nfloat z = tc.z;";
    auto result = FixImplicitConversions(in);
    // Declaration should become vec4
    CHECK(result.find("vec4 tc;") != std::string::npos);
}

TEST_CASE("vec2 upgraded to vec4 when .w accessed") {
    std::string in  = "vec2 uv;\nfloat w = uv.w;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("vec4 uv;") != std::string::npos);
}

TEST_CASE("vec2 with only .xy access NOT upgraded") {
    std::string in = "vec2 tc;\nfloat x = tc.x;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("vec2 tc;") != std::string::npos);
}

TEST_CASE("vec3 upgraded to vec4 when .w accessed") {
    std::string in  = "vec3 pos;\nfloat w = pos.w;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("vec4 pos;") != std::string::npos);
}

TEST_CASE("vec3 with only .xyz access NOT upgraded") {
    std::string in = "vec3 pos;\nfloat z = pos.z;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("vec3 pos;") != std::string::npos);
}

TEST_CASE("vec2 upgraded to vec4 with texture coord fixup") {
    std::string in  = "vec2 tc;\nfloat z = tc.z;\nvec4 s = texture(g_Tex, tc);";
    auto result = FixImplicitConversions(in);
    // After upgrade, texture() call should get .xy swizzle
    CHECK(result.find("texture(g_Tex, tc.xy)") != std::string::npos);
}

// --- Pattern 4: vec4→vec2, vec4→vec3, vec3→vec2 direct truncation ---

TEST_CASE("vec2 = vec4 gets .xy swizzle") {
    std::string in  = "vec2 a;\nvec4 b;\na = b;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a = b.xy;") != std::string::npos);
}

TEST_CASE("vec3 = vec4 gets .xyz swizzle") {
    std::string in  = "vec3 a;\nvec4 b;\na = b;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a = b.xyz;") != std::string::npos);
}

TEST_CASE("vec2 = vec3 gets .xy swizzle") {
    std::string in  = "vec2 a;\nvec3 b;\na = b;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a = b.xy;") != std::string::npos);
}

TEST_CASE("same-type assignment unchanged") {
    std::string in = "vec4 a;\nvec4 b;\na = b;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a = b;") != std::string::npos);
}

// --- Pattern 5: Arithmetic swizzle truncation ---

TEST_CASE("vec2 var + expr.xyzw → truncate to .xy") {
    std::string in  = "vec2 a = vec2(0);\nvec4 b;\na + b.xyzw;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a + b.xy") != std::string::npos);
}

TEST_CASE("expr.xyz + vec2 var → truncate to .xy") {
    std::string in  = "vec2 a = vec2(0);\nvec3 b;\nb.xyz + a;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("b.xy + a") != std::string::npos);
}

TEST_CASE("vec2 var + expr.xyz → truncate 3 to 2") {
    std::string in  = "vec2 a = vec2(0);\nvec3 b;\na + b.xyz;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a + b.xy") != std::string::npos);
    CHECK(result.find("a + b.xyz") == std::string::npos);
}

TEST_CASE("truncation preserves surrounding code") {
    std::string in = "float z = 1.0;\nvec2 uv = vec2(0);\nvec4 c;\nfloat w = uv + c.xyzw;\nfloat q = 2.0;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("uv + c.xy") != std::string::npos);
    CHECK(result.find("float z = 1.0") != std::string::npos);
    CHECK(result.find("float q = 2.0") != std::string::npos);
}

TEST_CASE("multiple truncations in same shader") {
    std::string in = "vec2 a = vec2(0);\nvec4 b;\na + b.xyzw;\na - b.xyzw;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a + b.xy") != std::string::npos);
    CHECK(result.find("a - b.xy") != std::string::npos);
}

TEST_CASE("multiple truncations preserve inter-match text exactly") {
    // Tests that position arithmetic (position()-lastPos, position()+length())
    // correctly preserves text between matches. If mutated, second match
    // would corrupt the inter-match text.
    std::string in = "vec2 a = vec2(0);\nvec4 b;\nint x = 42; a + b.xyzw; int y = 99; a * b.xyz; int z = 7;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("int x = 42; a + b.xy; int y = 99; a * b.xy; int z = 7;") != std::string::npos);
}

TEST_CASE("reverse truncation preserves surrounding code") {
    std::string in = "vec2 uv = vec2(0);\nvec4 c;\nfloat w = c.xyzw + uv;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("c.xy + uv") != std::string::npos);
}

TEST_CASE("vec2 with matching 2-component swizzle NOT truncated") {
    // sw > target_width loop: for vec2 (target=2), sw goes 4,3 — NOT 2
    // If mutated to >=, sw would also be 2, and .xy would be "truncated" to .xy (no change,
    // but the regex replacement would fire and potentially mangle the code)
    std::string in = "vec2 a = vec2(0);\nvec4 b;\na + b.xy;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a + b.xy") != std::string::npos);
    // Must NOT have been further truncated to .x
    CHECK(result.find("a + b.x;") == std::string::npos);
}

TEST_CASE("vec3 with matching 3-component swizzle NOT truncated") {
    std::string in = "vec3 a = vec3(0);\nvec4 b;\na + b.xyz;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a + b.xyz") != std::string::npos);
}

TEST_CASE("vec2 with 3-component swizzle truncated to 2") {
    std::string in = "vec2 a = vec2(0);\nvec4 b;\na + b.xyz;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a + b.xy") != std::string::npos);
}

// --- Pattern 6: Arithmetic swizzle expansion ---

TEST_CASE("vec3 var - expr.xx → expand to .xxx") {
    std::string in  = "vec3 a = vec3(0);\nvec4 b;\na - b.xx;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a - b.xxx") != std::string::npos);
}

TEST_CASE("expr.xy * vec4 var → expand to .xyyy") {
    std::string in  = "vec4 a = vec4(0);\nvec3 b;\nb.xy * a;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("b.xyyy * a") != std::string::npos);
}

TEST_CASE("expansion skipped when variable has swizzle suffix") {
    // If the vec3 var itself already has a swizzle (.xy), don't expand the RHS
    std::string in  = "vec3 a = vec3(0);\nvec4 b;\na.xy - b.xx;";
    auto result = FixImplicitConversions(in);
    // Should NOT expand to .xxx since a.xy makes it a vec2 operation
    CHECK(result.find("b.xxx") == std::string::npos);
}

TEST_CASE("reverse expansion: expr.xx * vec3 → expr.xxx") {
    std::string in  = "vec3 a = vec3(0);\nvec4 b;\nb.xx * a;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("b.xxx * a") != std::string::npos);
}

TEST_CASE("expansion repeats last char correctly") {
    // vec4 var - expr.st → expand to .sttt (last char 't' repeated twice)
    std::string in = "vec4 a = vec4(0);\nvec2 b;\na - b.st;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("a - b.sttt") != std::string::npos);
}

TEST_CASE("expansion preserves surrounding text exactly") {
    // This test checks substring extraction is exact — mutating position arithmetic corrupts output
    std::string in = "vec3 myVar = vec3(1);\nvec4 other;\nfloat f = 1.0; myVar * other.xy; float g = 2.0;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("myVar * other.xyy") != std::string::npos);
    CHECK(result.find("float f = 1.0") != std::string::npos);
    CHECK(result.find("float g = 2.0") != std::string::npos);
}

TEST_CASE("multiple expansions preserve inter-match text exactly") {
    std::string in = "vec3 a = vec3(0);\nvec4 b;\nint x = 1; a - b.xx; int y = 2; a + b.xy; int z = 3;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("int x = 1; a - b.xxx; int y = 2; a + b.xyy; int z = 3;") != std::string::npos);
}

TEST_CASE("expansion reverse: expr.xx OP vec3 exact output") {
    std::string in = "vec3 a = vec3(0);\nvec4 b;\nb.xx + a;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("b.xxx + a") != std::string::npos);
    // Must NOT corrupt surrounding code
    CHECK(result.find("vec3 a = vec3(0)") != std::string::npos);
}

// --- Pattern 7: vec3 = mat * vec4() → .xyz ---

TEST_CASE("vec3 = mat*vec4 gets .xyz truncation") {
    std::string in  = "vec3 pos = (g_ModelMatrix) * (vec4(v, 1.0)) ;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find(".xyz;") != std::string::npos);
}

TEST_CASE("vec4 = mat*vec4 unchanged") {
    std::string in = "vec4 pos = (g_ModelMatrix) * (vec4(v, 1.0)) ;";
    auto result = FixImplicitConversions(in);
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
    std::string in = "uniform float u_Scale;\nvec4 timer = texture(g_Texture0, uv);\nfloat off = u_Scale * timer;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("u_Scale * timer.x") != std::string::npos);
}

TEST_CASE("vec4 texture var with existing swizzle unchanged") {
    std::string in = "uniform float u_Scale;\nvec4 timer = texture(g_Texture0, uv);\nfloat off = u_Scale * timer.r;";
    auto result = FixImplicitConversions(in);
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
    std::string in  = "float x = (0 ? a : b);";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("bool(0)") != std::string::npos);
}

TEST_CASE("non-integer ternary condition unchanged") {
    std::string in = "float x = cond ? a : b;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("bool(") == std::string::npos);
}

// --- Pattern 12: comparison in arithmetic → float() ---

TEST_CASE("parenthesized comparison in arithmetic gets float() wrap") {
    std::string in  = "float x = (a > b) * c;";
    std::string out = "float x = float(a > b) * c;";
    CHECK_EQ(FixImplicitConversions(in), out);
}

TEST_CASE("comparison with <= gets float() wrap") {
    std::string in  = "float x = (a <= b) + d;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("float(a <= b)") != std::string::npos);
}

TEST_CASE("comparison without arithmetic unchanged") {
    std::string in = "bool x = (a > b);";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("float(a > b)") == std::string::npos);
}

// --- Pattern 13: bool var in compound assignment → float() ---

TEST_CASE("bool var after *= gets float() wrap") {
    std::string in  = "bool flag = true;\nfloat x = 1.0;\nx *= flag;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("*= float(flag)") != std::string::npos);
}

TEST_CASE("bool var after * gets float() wrap") {
    std::string in  = "bool flag = true;\nfloat x = 1.0 * flag;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("* float(flag)") != std::string::npos);
}

TEST_CASE("non-bool var in compound assignment unchanged") {
    std::string in = "float flag = 1.0;\nfloat x = 1.0;\nx *= flag;";
    auto result = FixImplicitConversions(in);
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
    std::string in  = "in vec4 v_TexCoord;\nvec4 s = texture(g_Tex, v_TexCoord);";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("texture(g_Tex, v_TexCoord.xy)") != std::string::npos);
}

TEST_CASE("texture with vec3 varying gets .xy") {
    std::string in  = "in vec3 v_UV;\nvec4 s = texture(g_Tex, v_UV);";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("texture(g_Tex, v_UV.xy)") != std::string::npos);
}

TEST_CASE("texture with vec2 varying unchanged") {
    std::string in = "in vec2 v_UV;\nvec4 s = texture(g_Tex, v_UV);";
    auto result = FixImplicitConversions(in);
    // vec2 is correct size — the "in vec[34]" regex doesn't match vec2
    CHECK(result.find("texture(g_Tex, v_UV)") != std::string::npos);
}

// --- Pattern 16: const TYPE = texture() → remove const ---

TEST_CASE("const vec4 = texture() removes const") {
    std::string in  = "const vec4 col = texture(g_Tex, uv);";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("const") == std::string::npos);
    CHECK(result.find("vec4 col = texture(g_Tex, uv)") != std::string::npos);
}

TEST_CASE("const float = texture() removes const") {
    std::string in  = "const float val = texture(g_Tex, uv);";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("const") == std::string::npos);
}

TEST_CASE("const with non-texture unchanged") {
    std::string in = "const vec4 col = vec4(1.0);";
    CHECK_EQ(FixImplicitConversions(in), in);
}

// --- Pattern 17: in-varying mutation → mutable copy ---

TEST_CASE("in varying with compound assignment gets mutable copy") {
    std::string in  = "in vec2 v_TexCoord;\nvoid main() {\nv_TexCoord += offset;\n}";
    auto result = FixImplicitConversions(in);
    // Should still have the 'in' declaration
    CHECK(result.find("in vec2 v_TexCoord;") != std::string::npos);
    // Should have a mutable copy
    CHECK(result.find("_m_v_TexCoord") != std::string::npos);
    CHECK(result.find("_m_v_TexCoord = v_TexCoord") != std::string::npos);
}

TEST_CASE("in varying without mutation unchanged") {
    std::string in = "in vec2 v_TexCoord;\nvoid main() {\nvec4 s = texture(g_Tex, v_TexCoord);\n}";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("_m_v_TexCoord") == std::string::npos);
}

// --- Pattern 18: vec3 = vec4() → .xyz ---

TEST_CASE("vec3 = vec4 constructor gets .xyz") {
    std::string in  = "vec3 pos = vec4(1.0, 2.0, 3.0, 4.0);";
    auto result = FixImplicitConversions(in);
    CHECK(result.find(".xyz;") != std::string::npos);
}

TEST_CASE("vec4 = vec4 constructor unchanged") {
    std::string in = "vec4 pos = vec4(1.0, 2.0, 3.0, 4.0);";
    auto result = FixImplicitConversions(in);
    CHECK(result.find(".xyz") == std::string::npos);
}

// --- Pattern 19: float = vec_uniform * expr → .x ---

TEST_CASE("float = vec_uniform * expr gets .x") {
    std::string in  = "uniform vec3 u_Color;\nfloat x = u_Color * 2.0;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("u_Color.x") != std::string::npos);
}

TEST_CASE("float = expr * vec_uniform gets .x") {
    std::string in  = "uniform vec4 u_Offset;\nfloat y = 3.0 * u_Offset;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("u_Offset.x;") != std::string::npos);
}

// --- Pattern 20: for-loop float-to-int ---

TEST_CASE("for loop int var from float expr gets int() cast") {
    std::string in  = "for (int i = -u_Count * 2; i <= u_Count * 2; i++)";
    auto result = FixImplicitConversions(in);
    bool found = result.find("int(-u_Count * 2)") != std::string::npos ||
                 result.find("int(- u_Count * 2)") != std::string::npos;
    CHECK(found);
}

TEST_CASE("for loop condition gets int() cast") {
    std::string in  = "i <= u_Count * 2;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("int(u_Count * 2)") != std::string::npos);
}

// --- Pattern 21: scalar assigned to glOutColor → vec4() ---

TEST_CASE("scalar expression assigned to glOutColor gets vec4() wrap") {
    std::string in  = "glOutColor = sample.x * mask;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("glOutColor = vec4(sample.x * mask)") != std::string::npos);
}

TEST_CASE("vec4() already on glOutColor unchanged") {
    std::string in = "glOutColor = vec4(1.0, 0.0, 0.0, 1.0);";
    CHECK_EQ(FixImplicitConversions(in), in);
}

TEST_CASE("glOutColor component write not wrapped") {
    std::string in = "glOutColor.a *= mask;";
    auto result = FixImplicitConversions(in);
    CHECK(result.find("vec4(") == std::string::npos);
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
    std::string in = "void main() {\nglOutColor.rgb = vec3(1.0);\n}";
    auto result = FixEffectAlpha(in);
    CHECK(result.find("glOutColor.a = texSample2D(g_Texture0, v_TexCoord.xy).a;") != std::string::npos);
}

TEST_CASE("component .r write without alpha → injects alpha") {
    std::string in = "void main() {\nglOutColor.r = 0.5;\n}";
    auto result = FixEffectAlpha(in);
    CHECK(result.find("glOutColor.a = texSample2D") != std::string::npos);
}

TEST_CASE("injection placed before closing brace") {
    std::string in = "void main() {\nglOutColor.rgb = vec3(1.0);\n}";
    auto result = FixEffectAlpha(in);
    auto alpha_pos = result.find("glOutColor.a = texSample2D");
    auto brace_pos = result.rfind('}');
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
    std::string in = "void main() {\n"
                     " albedo.a = (clamp(albedo.a + rays.a, 0.0, 1.0));\n"
                     " glOutColor = albedo;\n"
                     "}";
    auto result = FixCombineAlpha(in);
    CHECK(result.find("// albedo.a") != std::string::npos);
    CHECK(result.find("glOutColor = albedo;") != std::string::npos);
}

TEST_CASE("godrays_combine with extra whitespace") {
    std::string in = "void main() {\n"
                     " albedo.a = (clamp( albedo.a + rays.a , 0.0 , 1.0 )) ;\n"
                     " glOutColor = albedo;\n"
                     "}";
    auto result = FixCombineAlpha(in);
    CHECK(result.find("// albedo.a") != std::string::npos);
}

TEST_CASE("fluidsimulation_combine NOT matched (different LHS/operand)") {
    std::string in = "void main() {\n"
                     " albedo.a = (clamp(prev.a + albedo.a, 0.0, 1.0));\n"
                     " glOutColor = albedo;\n"
                     "}";
    auto result = FixCombineAlpha(in);
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
    std::string in = "void main() {\n"
                     " color.a = (clamp(color.a + bloom.a, 0.0, 1.0));\n"
                     " glOutColor = color;\n"
                     "}";
    auto result = FixCombineAlpha(in);
    CHECK(result.find("// color.a") != std::string::npos);
}

} // TEST_SUITE("FixCombineAlpha")

// ===========================================================================
// TranslateGeometryShader
// ===========================================================================

TEST_SUITE("TranslateGeometryShader") {

// --- Pattern 1: [maxvertexcount(N)] → #define + removal ---

TEST_CASE("maxvertexcount extracted to #define") {
    std::string in  = "[maxvertexcount(6)]\nvoid main() {}";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("#define WE_GS_MAX_VERTICES (6)") != std::string::npos);
    CHECK(result.find("[maxvertexcount") == std::string::npos);
}

TEST_CASE("maxvertexcount with expression") {
    std::string in  = "[maxvertexcount(SEGMENTS*2+2)]\nvoid main() {}";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("#define WE_GS_MAX_VERTICES (SEGMENTS*2+2)") != std::string::npos);
}

TEST_CASE("no maxvertexcount uses fallback 4") {
    std::string in  = "void main() {}";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("#define WE_GS_MAX_VERTICES (4)") != std::string::npos);
}

// --- Pattern 2: gl_Position declaration removal ---

TEST_CASE("in vec4 gl_Position removed") {
    std::string in  = "in vec4 gl_Position;\nvoid main() {}";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("in vec4 gl_Position;") == std::string::npos);
    CHECK(result.find("gl_Position input via gl_in") != std::string::npos);
}

TEST_CASE("out vec4 gl_Position removed") {
    std::string in  = "out vec4 gl_Position;\nvoid main() {}";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("out vec4 gl_Position;") == std::string::npos);
    CHECK(result.find("gl_Position output is built-in") != std::string::npos);
}

// --- Pattern 3: in TYPE v_xxx → in TYPE gs_in_v_xxx[] ---

TEST_CASE("input varying renamed to gs_in_ array") {
    std::string in  = "in vec4 v_Color;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("in vec4 gs_in_v_Color[];") != std::string::npos);
}

TEST_CASE("input float varying renamed") {
    std::string in  = "in float v_Alpha;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("in float gs_in_v_Alpha[];") != std::string::npos);
}

TEST_CASE("output varying NOT renamed") {
    std::string in  = "out vec4 v_Color;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("gs_in_v_Color") == std::string::npos);
}

// --- Pattern 4: IN[0].gl_Position → gl_in[0].gl_Position ---

TEST_CASE("IN[0].gl_Position → gl_in[0].gl_Position") {
    std::string in  = "vec4 pos = IN[0].gl_Position;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("gl_in[0].gl_Position") != std::string::npos);
    CHECK(result.find("IN[0].gl_Position") == std::string::npos);
}

// --- Pattern 5: IN[0].v_xxx → gs_in_v_xxx[0] ---

TEST_CASE("IN[0].v_Color → gs_in_v_Color[0]") {
    std::string in  = "vec4 c = IN[0].v_Color;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("gs_in_v_Color[0]") != std::string::npos);
    CHECK(result.find("IN[0].v_Color") == std::string::npos);
}

TEST_CASE("IN[0].v_TexCoord → gs_in_v_TexCoord[0]") {
    std::string in  = "vec2 uv = IN[0].v_TexCoord.xy;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("gs_in_v_TexCoord[0].xy") != std::string::npos);
}

// --- Pattern 6: PS_INPUT variable declaration removed ---

TEST_CASE("PS_INPUT v; removed") {
    std::string in  = "PS_INPUT v;\nvoid main() {}";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("PS_INPUT v;") == std::string::npos);
}

// --- Pattern 6a: PS_INPUT return type → void ---

TEST_CASE("PS_INPUT as function return type → void") {
    std::string in  = "PS_INPUT makeVertex() { }";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("void makeVertex()") != std::string::npos);
}

// --- Pattern 6b: VS_OUTPUT parameter removal ---

TEST_CASE("trailing VS_OUTPUT parameter removed") {
    std::string in  = "void emit(inout TriangleStream OUT, in VS_OUTPUT IN) {}";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("VS_OUTPUT") == std::string::npos);
    CHECK(result.find("inout TriangleStream OUT") != std::string::npos);
}

TEST_CASE("leading VS_OUTPUT parameter removed") {
    std::string in  = "void func(in VS_OUTPUT IN, inout TriangleStream OUT) {}";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("VS_OUTPUT") == std::string::npos);
}

// --- Pattern 6c: IN.gl_Position → gl_in[0].gl_Position ---

TEST_CASE("IN.gl_Position → gl_in[0].gl_Position") {
    std::string in  = "vec4 p = IN.gl_Position;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("gl_in[0].gl_Position") != std::string::npos);
}

// --- Pattern 6d: IN.v_xxx → gs_in_v_xxx[0] ---

TEST_CASE("IN.v_Color → gs_in_v_Color[0]") {
    std::string in  = "vec4 c = IN.v_Color;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("gs_in_v_Color[0]") != std::string::npos);
}

// --- Pattern 6e: bare IN[0] removal from function args ---

TEST_CASE("trailing IN[0] removed from args") {
    std::string in  = "func(OUT, IN[0]);";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("func(OUT)") != std::string::npos);
}

TEST_CASE("leading IN[0] removed from args") {
    std::string in  = "func(IN[0], OUT);";
    auto result = TranslateGeometryShader(in);
    bool found = result.find("func( OUT)") != std::string::npos ||
                 result.find("func(OUT)") != std::string::npos;
    CHECK(found);
}

// --- Pattern 6f: return v; → return; ---

TEST_CASE("return v; → return;") {
    std::string in  = "return v;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("return;") != std::string::npos);
    CHECK(result.find("return v;") == std::string::npos);
}

TEST_CASE("return value; not affected") {
    std::string in  = "return value;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("return value;") != std::string::npos);
}

// --- Pattern 7: v.gl_Position → gl_Position ---

TEST_CASE("v.gl_Position → gl_Position") {
    std::string in  = "v.gl_Position = vec4(1.0);";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("gl_Position = vec4(1.0)") != std::string::npos);
    CHECK(result.find("v.gl_Position") == std::string::npos);
}

// --- Pattern 8: v.v_xxx → v_xxx ---

TEST_CASE("v.v_Color → v_Color") {
    std::string in  = "v.v_Color = vec4(1.0);";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("v_Color = vec4(1.0)") != std::string::npos);
    CHECK(result.find("v.v_Color") == std::string::npos);
}

TEST_CASE("v.v_TexCoord.xy preserved swizzle") {
    std::string in  = "v.v_TexCoord.xy = uv;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("v_TexCoord.xy = uv") != std::string::npos);
}

// --- Pattern 8.5: vec3 varying = mul() → .xyz ---

TEST_CASE("v_WorldPos = mul() gets .xyz") {
    std::string in  = "v_WorldPos = mul(pos, mat);";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("v_WorldPos = (mul(pos, mat)).xyz;") != std::string::npos);
}

TEST_CASE("v_ScreenCoord = mul() gets .xyz") {
    std::string in  = "v_ScreenCoord = mul(pos, mat);";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find(".xyz;") != std::string::npos);
}

TEST_CASE("non-vec3 varying = mul() unchanged") {
    std::string in  = "v_Other = mul(pos, mat);";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find(".xyz") == std::string::npos);
}

// --- Pattern 9: OUT.Append() ---

TEST_CASE("OUT.Append(v) → EmitVertex()") {
    std::string in  = "OUT.Append(v);";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("EmitVertex();") != std::string::npos);
    CHECK(result.find("OUT.Append") == std::string::npos);
}

TEST_CASE("OUT.Append(FuncCall()) → FuncCall(); EmitVertex()") {
    std::string in  = "OUT.Append(makeVert(a, b));";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("makeVert(a, b); EmitVertex();") != std::string::npos);
}

TEST_CASE("OUT.Append with nested parens") {
    std::string in  = "OUT.Append(func(a, vec2(1.0, 2.0)));";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("EmitVertex()") != std::string::npos);
    CHECK(result.find("func(a, vec2(1.0, 2.0))") != std::string::npos);
}

TEST_CASE("OUT.Append with space before semicolon") {
    std::string in  = "OUT.Append(makeVert()) ;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("makeVert(); EmitVertex();") != std::string::npos);
    CHECK(result.find("OUT.Append") == std::string::npos);
}

TEST_CASE("OUT.Append preserves code before and after") {
    std::string in  = "float x = 1.0;\nOUT.Append(buildVert(a, b));\nfloat y = 2.0;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("float x = 1.0") != std::string::npos);
    CHECK(result.find("float y = 2.0") != std::string::npos);
    CHECK(result.find("buildVert(a, b); EmitVertex();") != std::string::npos);
}

TEST_CASE("multiple OUT.Append in same shader") {
    std::string in  = "OUT.Append(makeA());\nOUT.Append(makeB());";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("makeA(); EmitVertex();") != std::string::npos);
    CHECK(result.find("makeB(); EmitVertex();") != std::string::npos);
}

// --- Pattern 10: OUT.RestartStrip() → EndPrimitive() ---

TEST_CASE("OUT.RestartStrip() → EndPrimitive()") {
    std::string in  = "OUT.RestartStrip();";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("EndPrimitive();") != std::string::npos);
    CHECK(result.find("OUT.RestartStrip") == std::string::npos);
}

// --- Pattern 11: for-loop variable shadowing ---

TEST_CASE("for loop int s shadowing float s renamed") {
    std::string in  = "for(int s=0;s<N;++s){float s=smoothstep(0.0,1.0,t);}";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("_si") != std::string::npos);
    CHECK(result.find("float s") != std::string::npos);
}

TEST_CASE("for loop int s without float s unchanged") {
    std::string in  = "for(int s=0;s<N;++s){float t=1.0;}";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("_si") == std::string::npos);
}

// --- Pattern 12: gl_in[0].gl_Position vec4→vec3 truncation for function args ---

TEST_CASE("gl_in[0].gl_Position truncated to .xyz for vec3 param") {
    std::string in  = "vec3 doSomething(vec3 pos) { return pos; }\nvoid main() { doSomething(gl_in[0].gl_Position); }";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("gl_in[0].gl_Position.xyz") != std::string::npos);
}

TEST_CASE("gl_in[0].gl_Position NOT truncated for vec4 param") {
    std::string in  = "void doSomething(vec4 pos) { }\nvoid main() { doSomething(gl_in[0].gl_Position); }";
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
    std::string in = "vec3 doSomething(float f, vec3 pos) { return pos; }\nvoid main() { doSomething(1.0, gl_in[0].gl_Position); }";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("gl_in[0].gl_Position.xyz") != std::string::npos);
}

TEST_CASE("gl_in[0].gl_Position in nested call correctly scanned") {
    std::string in = "vec3 outer(vec3 p) { return p; }\nvoid main() { outer(gl_in[0].gl_Position); }";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("gl_in[0].gl_Position.xyz") != std::string::npos);
}

TEST_CASE("multiple gl_in[0].gl_Position occurrences all processed") {
    std::string in = "vec3 f(vec3 p) { return p; }\nvoid main() { f(gl_in[0].gl_Position); f(gl_in[0].gl_Position); }";
    auto result = TranslateGeometryShader(in);
    // Count occurrences of .xyz
    size_t count = 0;
    size_t pos = 0;
    while ((pos = result.find("gl_in[0].gl_Position.xyz", pos)) != std::string::npos) {
        count++;
        pos += 24;
    }
    CHECK(count == 2);
}

TEST_CASE("gl_in[0].gl_Position with existing swizzle not touched") {
    std::string in  = "vec3 doSomething(vec3 pos) { return pos; }\nvoid main() { doSomething(gl_in[0].gl_Position.xyz); }";
    auto result = TranslateGeometryShader(in);
    // Should not double-swizzle
    CHECK(result.find("gl_in[0].gl_Position.xyz.xyz") == std::string::npos);
    CHECK(result.find("gl_in[0].gl_Position.xyz") != std::string::npos);
}

// --- Combined test: full geometry shader translation ---

TEST_CASE("combined geometry shader translation") {
    std::string in =
        "[maxvertexcount(4)]\n"
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
    auto result = TranslateGeometryShader(in);
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
    std::string in  = "int a=1; OUT.Append(f1()); int b=2; OUT.Append(f2()); int c=3;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("int a=1; f1(); EmitVertex(); int b=2; f2(); EmitVertex(); int c=3;") != std::string::npos);
}

TEST_CASE("OUT.Append exact extraction: no extra chars leaked") {
    // Tests that start-pos subtraction and i-1-inner are exact
    std::string in  = "AAA OUT.Append(compute(x, y)); BBB";
    auto result = TranslateGeometryShader(in);
    // Result should be: "AAA compute(x, y); EmitVertex(); BBB"
    CHECK(result.find("AAA compute(x, y); EmitVertex(); BBB") != std::string::npos);
}

TEST_CASE("OUT.Append with deeply nested parens") {
    std::string in  = "OUT.Append(a(b(c(d))));";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("a(b(c(d))); EmitVertex();") != std::string::npos);
}

TEST_CASE("OUT.Append without semicolon") {
    std::string in  = "OUT.Append(makeVert())\nnext line";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("makeVert(); EmitVertex()") != std::string::npos);
    CHECK(result.find("next line") != std::string::npos);
}

TEST_CASE("OUT.Append with tab before semicolon") {
    std::string in  = "OUT.Append(v)\t;";
    auto result = TranslateGeometryShader(in);
    CHECK(result.find("EmitVertex();") != std::string::npos);
}

TEST_CASE("gl_in[0].gl_Position with existing swizzle not truncated") {
    std::string in = "vec3 f(vec3 p) { return p; }\nvoid main() { f(gl_in[0].gl_Position.xyz); }";
    auto result = TranslateGeometryShader(in);
    // Should keep .xyz, NOT add another .xyz
    auto pos = result.find("gl_in[0].gl_Position.xyz");
    CHECK(pos != std::string::npos);
    // No double .xyz.xyz
    CHECK(result.find("gl_in[0].gl_Position.xyz.xyz") == std::string::npos);
}

TEST_CASE("gl_in[0].gl_Position multiple occurrences") {
    std::string in = "vec3 f(vec3 a, vec3 b) { return a; }\n"
                     "void main() { f(gl_in[0].gl_Position, gl_in[0].gl_Position); }";
    auto result = TranslateGeometryShader(in);
    // Both should get .xyz
    size_t first  = result.find("gl_in[0].gl_Position.xyz");
    size_t second = result.find("gl_in[0].gl_Position.xyz", first + 1);
    CHECK(first  != std::string::npos);
    CHECK(second != std::string::npos);
}

} // TEST_SUITE("TranslateGeometryShader")
