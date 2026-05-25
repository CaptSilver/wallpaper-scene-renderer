#pragma once
// Pure string→string shader transform functions extracted from WPShaderParser.cpp.
// Header-only; depends only on the C++ standard library (+Logging.h for FixEffectAlpha
// diagnostics).

#include <atomic>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "Utils/Logging.h"

// ---------------------------------------------------------------------------
// regexTransformAll — apply a regex to a string, replacing each match via a
// user-supplied functor.  Consolidates the sregex_iterator + lastPos pattern
// that repeats throughout the shader transforms below.
// ---------------------------------------------------------------------------
template<typename ReplacerFn>
inline int regexTransformAll(std::string& text, const std::regex& re, ReplacerFn replacer) {
    std::string tmp;
    size_t      lastPos = 0;
    int         count   = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator();
         ++it) {
        tmp.append(text, lastPos, (size_t)(*it).position() - lastPos);
        tmp += replacer(*it);
        lastPos = (size_t)(*it).position() + (*it).length();
        ++count;
    }
    if (count > 0) {
        tmp.append(text, lastPos, std::string::npos);
        text = std::move(tmp);
    }
    return count;
}

// ---------------------------------------------------------------------------
// findMatchingParen — find the position after the closing ')' that matches
// the '(' at position `openPos`.  Returns npos if unmatched.
// ---------------------------------------------------------------------------
inline size_t findMatchingParen(const std::string& text, size_t openPos) {
    int    depth = 1;
    size_t i     = openPos + 1;
    for (; i < text.size() && depth > 0; ++i) {
        if (text[i] == '(')
            ++depth;
        else if (text[i] == ')')
            --depth;
    }
    return (depth == 0) ? i : std::string::npos;
}

// ---------------------------------------------------------------------------
// skipWhitespaceAndSemicolon — advance past optional whitespace + semicolon.
// ---------------------------------------------------------------------------
inline size_t skipWhitespaceAndSemicolon(const std::string& text, size_t pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    if (pos < text.size() && text[pos] == ';') ++pos;
    return pos;
}

// ---------------------------------------------------------------------------
// findEnclosingCallInfo — given a position inside a function call, scan backward
// to find the enclosing '(' and extract function name + argument index.
// Returns {funcName, argIndex} or {"", -1} if not inside a function call.
// ---------------------------------------------------------------------------
struct CallInfo {
    std::string funcName;
    int         argIndex { -1 };
};
inline CallInfo findEnclosingCallInfo(const std::string& text, size_t innerPos) {
    size_t scan  = innerPos;
    int    depth = 0;
    while (scan > 0) {
        --scan;
        if (text[scan] == ')')
            ++depth;
        else if (text[scan] == '(') {
            if (depth == 0) {
                // Count commas between '(' and innerPos at depth 0
                int argIdx = 0, cd = 0;
                for (size_t j = scan + 1; j < innerPos; ++j) {
                    if (text[j] == '(')
                        ++cd;
                    else if (text[j] == ')')
                        --cd;
                    else if (text[j] == ',' && cd == 0)
                        ++argIdx;
                }
                // Extract function name before '('
                size_t ne = scan;
                while (ne > 0 && std::isspace(text[ne - 1])) --ne;
                size_t ns = ne;
                while (ns > 0 && (std::isalnum(text[ns - 1]) || text[ns - 1] == '_')) --ns;
                return { text.substr(ns, ne - ns), argIdx };
            }
            --depth;
        }
    }
    return { "", -1 };
}

// ---------------------------------------------------------------------------
// NeedsFlatDecoration
// ---------------------------------------------------------------------------
// Check if a GLSL I/O declaration has an integer type that requires flat interpolation.
// Per Vulkan spec, integer/double fragment inputs must be decorated "flat".
inline bool NeedsFlatDecoration(const std::string& decl) {
    // Match integer types: int, uint, ivec2-4, uvec2-4
    static const std::regex re_int_type(R"(\b(int|uint|ivec[234]|uvec[234])\b)");
    // Already has flat qualifier
    static const std::regex re_has_flat(R"(\bflat\b)");
    return std::regex_search(decl, re_int_type) && ! std::regex_search(decl, re_has_flat);
}

// ---------------------------------------------------------------------------
// TranslateGeometryShader
// ---------------------------------------------------------------------------
// Translate HLSL-style geometry shader source into GLSL-compatible
// shader constructs (gl_in, EmitVertex, layout declarations).
inline std::string TranslateGeometryShader(const std::string& src) {
    std::string result = src;

    // 1. Remove [maxvertexcount(EXPR)] — layout declarations will be added
    //    after preprocessing when the expression is fully resolved.
    //    Store the expression as a #define for the preprocessor to expand.
    {
        std::regex  re(R"(\[maxvertexcount\(([^)]+)\)\])");
        std::smatch m;
        std::string maxVertExpr = "4"; // fallback
        if (std::regex_search(result, m, re)) {
            maxVertExpr = m[1].str();
        }
        result = std::regex_replace(result, re, "");
        // Prepend #define so the preprocessor evaluates the expression.
        // The actual layout() declaration is injected post-preprocessing.
        result = "#define WE_GS_MAX_VERTICES (" + maxVertExpr + ")\n" + result;
    }

    // 2. Remove "in vec4 gl_Position;" and "out vec4 gl_Position;" declarations.
    //    gl_Position is a built-in; re-declaring it causes errors.
    {
        std::regex re_in_gl(R"(\bin\s+vec4\s+gl_Position\s*;)");
        std::regex re_out_gl(R"(\bout\s+vec4\s+gl_Position\s*;)");
        result = std::regex_replace(result, re_in_gl, "// (gl_Position input via gl_in)");
        result = std::regex_replace(result, re_out_gl, "// (gl_Position output is built-in)");
    }

    // 3. Convert input declarations to arrays with gs_in_ prefix to avoid
    //    same-name redefinition (GLSL forbids same-name in/out):
    //    "in TYPE v_xxx;" → "in TYPE gs_in_v_xxx[];"
    {
        std::regex re_in_decl(R"(\bin\s+(vec[234]|float|int|uint|ivec[234]|uvec[234]|mat[234])"
                              R"()\s+(v_\w+)\s*;)");
        result = std::regex_replace(result, re_in_decl, "in $1 gs_in_$2[];");
    }

    // 4. Replace IN[0].gl_Position → gl_in[0].gl_Position
    {
        std::regex re(R"(\bIN\[0\]\.gl_Position\b)");
        result = std::regex_replace(result, re, "gl_in[0].gl_Position");
    }

    // 5. Replace IN[0].v_xxx → gs_in_v_xxx[0]  (access prefixed geometry input)
    {
        std::regex re(R"(\bIN\[0\]\.(v_\w+)\b)");
        result = std::regex_replace(result, re, "gs_in_$1[0]");
    }

    // 6. Remove PS_INPUT variable declaration: "PS_INPUT v;"
    {
        std::regex re(R"(\bPS_INPUT\s+\w+\s*;)");
        result = std::regex_replace(result, re, "");
    }

    // 6a. Replace remaining PS_INPUT (function return type) → void
    {
        std::regex re(R"(\bPS_INPUT\b)");
        result = std::regex_replace(result, re, "void");
    }

    // 6b. Remove VS_OUTPUT parameter from function signatures
    //     Handles ", in VS_OUTPUT IN" and "in VS_OUTPUT IN, "
    {
        std::regex re1(R"(,\s*in\s+VS_OUTPUT\s+IN\b)");
        result = std::regex_replace(result, re1, "");
        std::regex re2(R"(\bin\s+VS_OUTPUT\s+IN\s*,\s*)");
        result = std::regex_replace(result, re2, "");
    }

    // 6c. Translate function-local IN.gl_Position → gl_in[0].gl_Position
    {
        std::regex re(R"(\bIN\.gl_Position\b)");
        result = std::regex_replace(result, re, "gl_in[0].gl_Position");
    }

    // 6d. Translate function-local IN.v_xxx → gs_in_v_xxx[0]
    {
        std::regex re(R"(\bIN\.(v_\w+)\b)");
        result = std::regex_replace(result, re, "gs_in_$1[0]");
    }

    // 6e. Remove bare IN[0] from function call arguments
    {
        std::regex re1(R"(,\s*IN\[0\])");
        result = std::regex_replace(result, re1, "");
        std::regex re2(R"(IN\[0\]\s*,\s*)");
        result = std::regex_replace(result, re2, "");
    }

    // 6f. Replace "return v;" → "return;" (void function)
    {
        std::regex re(R"(\breturn\s+v\s*;)");
        result = std::regex_replace(result, re, "return;");
    }

    // 7. Replace "v.gl_Position" → "gl_Position" (built-in output)
    {
        std::regex re(R"(\bv\.gl_Position\b)");
        result = std::regex_replace(result, re, "gl_Position");
    }

    // 8. Replace "v.v_xxx" → "v_xxx" (output varying assignment)
    //    Handles v.v_Color, v.v_TexCoord.xy, v.v_ViewDir.xyz, etc.
    {
        std::regex re(R"(\bv\.(v_\w+)\b)");
        result = std::regex_replace(result, re, "$1");
    }

    // 8.5. Truncate vec4 to vec3 for assignments to known vec3 output varyings.
    // HLSL implicitly truncates; GLSL requires .xyz.
    // Target only mul() expressions which return vec4 in our dialect.
    {
        // Handle v_WorldPos, v_WorldRight, v_ScreenCoord (all vec3)
        // Ensure we don't match if it already has .xyz
        std::regex re(
            R"(\b(v_WorldPos|v_WorldRight|v_ScreenCoord)\s*=\s*(mul\s*\([^;]+?\))\s*;(?!\s*\.xyz))");
        result = std::regex_replace(result, re, "$1 = ($2).xyz;");
    }

    // 9. Replace OUT.Append(expr); → expr; EmitVertex();
    //    Handles both OUT.Append(v); and OUT.Append(FuncCall(...));
    //    Uses balanced-paren matching for nested calls.
    {
        const std::string marker = "OUT.Append(";
        std::string       replaced;
        size_t            pos = 0;
        while (true) {
            size_t start = result.find(marker, pos);
            if (start == std::string::npos) {
                replaced.append(result, pos, std::string::npos);
                break;
            }
            replaced.append(result, pos, start - pos);
            // inner starts after "OUT.Append(" — the opening paren is at start+marker.size()-1
            size_t openParen  = start + marker.size() - 1;
            size_t afterClose = findMatchingParen(result, openParen);
            if (afterClose == std::string::npos) {
                // Unmatched paren — keep original text
                replaced.append(result, start, marker.size());
                pos = start + marker.size();
                continue;
            }
            size_t inner    = openParen + 1;
            size_t innerEnd = afterClose - 1; // position of ')'
            size_t after    = skipWhitespaceAndSemicolon(result, afterClose);

            std::string innerExpr = result.substr(inner, innerEnd - inner);
            // If inner expr is just a bare identifier (the old PS_INPUT var),
            // skip it — the output varyings are already set directly.
            if (std::regex_match(innerExpr, std::regex(R"(\s*\w+\s*)")))
                replaced += "EmitVertex();";
            else
                replaced += innerExpr + "; EmitVertex();";
            pos = after;
        }
        result = std::move(replaced);
    }

    // 10. Replace OUT.RestartStrip(); → EndPrimitive();
    {
        std::regex re(R"(\bOUT\.RestartStrip\s*\(\s*\)\s*;)");
        result = std::regex_replace(result, re, "EndPrimitive();");
    }

    // 11. Fix: HLSL variable shadowing in for-loops.
    // WE's genericropeparticle.geom has: for(int s=0;s<N;++s){float s=smoothstep(...);}
    // HLSL allows inner scope to shadow outer; GLSL does not (redefinition error).
    // Rename the loop counter: int s → int _si (keeps float s inside body valid).
    {
        std::regex re_for(R"(for\s*\(\s*int\s+s\s*=)");
        if (std::regex_search(result, re_for) && result.find("float s") != std::string::npos) {
            std::regex re_header(
                R"(for\s*\(\s*int\s+s\s*=\s*(\d+)\s*;\s*s\s*(<\s*[^;]+);\s*\+\+\s*s\s*\))");
            result = std::regex_replace(result, re_header, "for (int _si = $1; _si $2; ++ _si)");
        }
    }

    // 12. gl_in[0].gl_Position is vec4; HLSL implicitly truncates vec4→vec3 in
    //     function args. Only add .xyz when the function parameter is vec3 (not vec4).
    {
        // Collect function declarations: funcName → [paramType0, paramType1, ...]
        std::map<std::string, std::vector<std::string>> funcSigs;
        {
            std::regex re(
                R"(\b(?:void|float|vec[234]|int|uint|bool|mat[234])\s+(\w+)\s*\(([^)]*)\))");
            std::regex paramRe(R"((?:in\s+|out\s+|inout\s+|const\s+)*(\w+)\s+\w+)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                 it != std::sregex_iterator();
                 ++it) {
                std::string              paramStr = (*it)[2].str();
                std::vector<std::string> types;
                for (auto pit = std::sregex_iterator(paramStr.begin(), paramStr.end(), paramRe);
                     pit != std::sregex_iterator();
                     ++pit)
                    types.push_back((*pit)[1].str());
                funcSigs[(*it)[1].str()] = types;
            }
        }

        const std::string gl_pos = "gl_in[0].gl_Position";
        std::string       out;
        size_t            pos = 0;
        while (pos < result.size()) {
            size_t found = result.find(gl_pos, pos);
            if (found == std::string::npos) {
                out.append(result, pos, std::string::npos);
                break;
            }
            size_t after = found + gl_pos.size();
            // If followed by . or alnum (already has swizzle), skip
            if (after < result.size() &&
                (result[after] == '.' || std::isalnum(result[after]) || result[after] == '_')) {
                out.append(result, pos, after - pos);
                pos = after;
                continue;
            }

            // Find enclosing function call and check if parameter is vec3
            bool needsTrunc = false;
            auto callInfo   = findEnclosingCallInfo(result, found);
            if (! callInfo.funcName.empty() && funcSigs.count(callInfo.funcName)) {
                auto& p = funcSigs[callInfo.funcName];
                if (callInfo.argIndex < (int)p.size() && p[callInfo.argIndex] == "vec3")
                    needsTrunc = true;
            }

            out.append(result, pos, found - pos);
            out += gl_pos;
            if (needsTrunc) out += ".xyz";
            pos = after;
        }
        result = std::move(out);
    }

    return result;
}

// ---------------------------------------------------------------------------
// FixImplicitConversions
// ---------------------------------------------------------------------------
// Fix HLSL->GLSL implicit type conversion issues.
// HLSL allows implicit float<->int conversions, GLSL/SPIR-V does not.
inline std::string FixImplicitConversions(const std::string& src) {
    std::string result = src;

    // Fix: "float VAR = int(EXPR)" -> "int VAR = int(EXPR)"
    // HLSL pattern where a float variable is assigned an int constructor result,
    // then used in int contexts (for loops)
    {
        static const std::regex re(R"((\bfloat\s+)(\w+)(\s*=\s*int\s*\())");
        result = std::regex_replace(result, re, "int $2$3");
    }

    // Fix: IDENTIFIER % int_literal -> int(IDENTIFIER) % int_literal
    // HLSL allows % on floats; GLSL requires integer operands for %.
    // When the result is assigned directly to a uint variable, additionally wrap in
    // uint() to avoid the secondary int→uint implicit conversion error.
    {
        // Case: uint VAR = (WORD OP int_lit) % N  e.g. "uint b = (a + 1) % 32;"
        // where WORD is uint: "uint + int" and "uint % int" are both GLSL errors.
        // Fix: uint VAR = uint((int(WORD) OP int_lit) % N)
        {
            static const std::regex re(R"(\buint\s+(\w+)\s*=\s*\((\w+)\s*([\+\-])\s*(\d+)\)\s*%\s*(\d+\b))");
            result = std::regex_replace(result, re, "uint $1 = uint((int($2) $3 $4) % $5)");
        }
        // Special case: uint VAR = EXPR % N;  →  uint VAR = uint(int(EXPR) % N);
        {
            static const std::regex re(R"(\buint\s+(\w+)\s*=\s*\b(\w+)\s*%\s*(\d+\b))");
            result = std::regex_replace(result, re, "uint $1 = uint(int($2) % $3)");
        }
        // General case: EXPR % N  →  int(EXPR) % N
        {
            static const std::regex re(R"(\b(\w+)\s*%\s*(\d+\b))");
            result = std::regex_replace(result, re, "int($1) % $2");
        }
    }

    // Fix: HLSL varyings declared as vecN but accessed with components beyond N.
    // In DirectX, texture-coordinate interpolator slots are always 4-wide regardless of
    // the declared float2/float3 type; HLSL shaders rely on this.  GLSL enforces the
    // declared width strictly, so "in vec2 v_TexCoord; ... v_TexCoord.zw" is an error.
    // Upgrade vec2 → vec4 when .z/.w (xyzw) or .b/.a (rgba) is accessed on it;
    // likewise vec3 → vec4 when .w/.a is accessed.
    // NOTE: must run before fixTrunc so that upgraded variables are not incorrectly
    // truncated (e.g. a vec2 upgraded to vec4 must not have its assignments cut to .xy).
    {
        auto upgradeIfOutOfRange = [&result](const char* small_type,
                                             const char* big_type,
                                             const char* /*oob_pattern*/,
                                             const char* bare_swizzle) {
            static const std::regex re_decl_vec2(R"(\bvec2\s+(\w+)\s*;)");
            static const std::regex re_decl_vec3(R"(\bvec3\s+(\w+)\s*;)");
            static const std::regex re_decl_vec4(R"(\bvec4\s+(\w+)\s*;)");
            const std::regex&       re_decl = (small_type[3] == '2') ? re_decl_vec2
                                            : (small_type[3] == '3') ? re_decl_vec3
                                                                      : re_decl_vec4;
            // Collect all names that are declared as small_type (via the hoisted re_decl).
            std::set<std::string> decl_names;
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_decl);
                 it != std::sregex_iterator();
                 ++it)
                decl_names.insert((*it)[1].str());

            // Scan result once for OOB-accessed names (constant pattern, no per-name NFA).
            // vec2 oob_pattern = \.[zwba]  →  captures any identifier accessed via .z/.w/.b/.a
            // vec3 oob_pattern = \.[wa]    →  captures any identifier accessed via .w/.a
            static const std::regex re_oob_vec2(R"(\b(\w+)\.[zwba])");
            static const std::regex re_oob_vec3(R"(\b(\w+)\.[wa])");
            const std::regex&       re_oob = (small_type[3] == '2') ? re_oob_vec2 : re_oob_vec3;
            std::set<std::string>   oob_accessed;
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_oob);
                 it != std::sregex_iterator();
                 ++it)
                oob_accessed.insert((*it)[1].str());

            // to_upgrade = declared as small_type AND accessed out-of-range.
            std::set<std::string> to_upgrade;
            for (const auto& n : decl_names)
                if (oob_accessed.count(n))
                    to_upgrade.insert(n);

            if (to_upgrade.empty()) return;

            // Upgrade declarations: one pass over result, filter by to_upgrade membership.
            // Replaces the leading small_type token with big_type; remainder of match preserved.
            const std::size_t type_len = std::string(small_type).size();
            regexTransformAll(result, re_decl, [&](const std::smatch& m) -> std::string {
                if (!to_upgrade.count(m[1].str())) return m[0].str();
                return std::string(big_type) + m[0].str().substr(type_len);
            });

            // Fix bare texture() coord uses: one pass, filter coord arg by to_upgrade.
            // texture(sampler, NAME) → texture(sampler, NAME.bare_swizzle)
            static const std::regex re_tex_coord(
                R"(\btexture\s*\(\s*(\w+)\s*,\s*(\w+)\s*\))");
            regexTransformAll(result, re_tex_coord, [&](const std::smatch& m) -> std::string {
                const std::string coord = m[2].str();
                if (!to_upgrade.count(coord)) return m[0].str();
                return "texture(" + m[1].str() + ", " + coord + "." + bare_swizzle + ")";
            });
        };
        upgradeIfOutOfRange("vec2", "vec4", R"(\.[zwba])", "xy");
        upgradeIfOutOfRange("vec3", "vec4", R"(\.[wa])", "xyz");
    }

    // Fix: HLSL implicit vector truncation (vec4->vec2, vec4->vec3, vec3->vec2).
    // HLSL allows "vec2_var = vec4_expr" silently dropping the extra components;
    // GLSL requires an explicit swizzle.  We collect all declared variable names
    // per width and rewrite bare same-name assignments.
    // NOTE: runs after upgradeIfOutOfRange so that variables already upgraded to vec4
    // are not seen as vec2/vec3 targets and have their assignments incorrectly truncated.
    {
        auto collect = [&result](const char* type) {
            static const std::regex re_vec2(R"(\bvec2\s+(\w+)\b)");
            static const std::regex re_vec3(R"(\bvec3\s+(\w+)\b)");
            static const std::regex re_vec4(R"(\bvec4\s+(\w+)\b)");
            const std::regex&       re = (type[3] == '2') ? re_vec2
                                       : (type[3] == '3') ? re_vec3
                                                          : re_vec4;
            std::set<std::string> vars;
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                 it != std::sregex_iterator();
                 ++it)
                vars.insert((*it)[1].str());
            return vars;
        };

        const auto vec2_vars = collect("vec2");
        const auto vec3_vars = collect("vec3");
        const auto vec4_vars = collect("vec4");

        auto fixTrunc = [&result](const std::set<std::string>& dst,
                                  const std::set<std::string>& src,
                                  const char*                  swizzle) {
            // One constant pattern captures any `word = word ;` assignment.
            // The replacer filters on: group1 in dst, group2 in src, group1 ≠ group2,
            // and group1 not in src (the shadowing guard — same name at two widths
            // means we can't tell which declaration the assignment refers to, so
            // we refuse to truncate; driver: Game Of Life canvas_paint.frag —
            // `vec4 cursor` in main(), `vec2 cursor` parameter in calcStrokeInfluence).
            static const std::regex re(R"(\b(\w+)\s*=\s*(\w+)\s*;)");
            std::string             sw(swizzle);
            regexTransformAll(result, re, [&](const std::smatch& m) -> std::string {
                const std::string& d = m[1].str();
                const std::string& s = m[2].str();
                if (dst.count(d) && src.count(s) && d != s && !src.count(d))
                    return d + " = " + s + "." + sw + ";";
                return m[0].str();
            });
        };

        fixTrunc(vec2_vars, vec3_vars, "xy");
        fixTrunc(vec2_vars, vec4_vars, "xy");
        fixTrunc(vec3_vars, vec4_vars, "xyz");

        // Fix: HLSL implicit vector truncation in arithmetic expressions.
        // HLSL allows vec2 + vec4 (truncates the larger); GLSL requires matching
        // dimensions.  When a known vecN variable is in arithmetic with a swizzle
        // of more than N components, truncate the swizzle to N.
        auto fixArithSwizzleTrunc = [&result](const std::set<std::string>& small_vars,
                                              int                          target_width) {
            // One static forward+reverse pair per swizzle width (sw ∈ {3,4}).
            // Captures any \w+ identifier; C++ filter checks group1/group4 ∈ small_vars.
            static const std::regex re_fwd4(
                R"(\b(\w+)(\s*[+\-*/]\s*)(\w+)\.([xyzwrgbastpq]{4})\b)");
            static const std::regex re_rev4(
                R"((\w+)\.([xyzwrgbastpq]{4})(\s*[+\-*/]\s*)\b(\w+)\b)");
            static const std::regex re_fwd3(
                R"(\b(\w+)(\s*[+\-*/]\s*)(\w+)\.([xyzwrgbastpq]{3})\b)");
            static const std::regex re_rev3(
                R"((\w+)\.([xyzwrgbastpq]{3})(\s*[+\-*/]\s*)\b(\w+)\b)");

            for (int sw = 4; sw > target_width; --sw) {
                const std::regex& re_fwd = (sw == 4) ? re_fwd4 : re_fwd3;
                const std::regex& re_rev = (sw == 4) ? re_rev4 : re_rev3;

                // VAR OP WORD.XXXX — filter: group1 (VAR) must be in small_vars
                regexTransformAll(result, re_fwd, [&](const std::smatch& m) -> std::string {
                    if (!small_vars.count(m[1].str())) return m[0].str();
                    return m[1].str() + m[2].str() + m[3].str() + "." +
                           m[4].str().substr(0, target_width);
                });

                // Reverse: WORD.XXXX OP VAR — filter: group4 (VAR) must be in small_vars
                regexTransformAll(result, re_rev, [&](const std::smatch& m) -> std::string {
                    if (!small_vars.count(m[4].str())) return m[0].str();
                    return m[1].str() + "." + m[2].str().substr(0, target_width) +
                           m[3].str() + m[4].str();
                });
            }
        };
        // For arithmetic truncation, only consider local variable declarations
        // (TYPE NAME ; or TYPE NAME =), not function parameters (TYPE NAME , or TYPE NAME )).
        // This avoids false positives when the same name appears as vec2 param + vec4 local.
        auto collectLocal = [&result](const char* type) {
            static const std::regex re_vec2(R"(\bvec2\s+(\w+)\s*[;=])");
            static const std::regex re_vec3(R"(\bvec3\s+(\w+)\s*[;=])");
            static const std::regex re_vec4(R"(\bvec4\s+(\w+)\s*[;=])");
            const std::regex&       re = (type[3] == '2') ? re_vec2
                                       : (type[3] == '3') ? re_vec3
                                                          : re_vec4;
            std::set<std::string> vars;
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                 it != std::sregex_iterator();
                 ++it)
                vars.insert((*it)[1].str());
            return vars;
        };
        const auto local_vec2 = collectLocal("vec2");
        const auto local_vec3 = collectLocal("vec3");
        // Skip shadowed names from Trunc: if a name is declared at multiple
        // widths in different scopes, truncating its swizzle for one scope
        // can break the other.  Daisies (3501635854) ships `vec3 p` in
        // hsv2rgb AND `vec2 p` in main; the main scope's `p + e.xyy` needs
        // the swizzle truncated to `e.xy`, but the hsv2rgb scope's
        // `p - K.xx` needs `K.xx` expanded to `K.xxx`.  Letting Trunc fire
        // for both `p` interpretations corrupts one.  Skip — my vec2-to-
        // vec3 broadcast pass (further down) handles the main-scope case
        // by wrapping the vec2 in `vec3(p, 0.0)`.
        auto withoutShadowed = [](const std::set<std::string>& src,
                                  const std::set<std::string>& shadow) {
            std::set<std::string> out;
            for (const auto& s : src)
                if (! shadow.count(s)) out.insert(s);
            return out;
        };
        const auto trunc_vec2 = withoutShadowed(local_vec2, local_vec3);
        const auto trunc_vec3 = withoutShadowed(local_vec3, local_vec2);
        fixArithSwizzleTrunc(trunc_vec2, 2);
        fixArithSwizzleTrunc(trunc_vec3, 3);

        // Fix: HLSL implicit vector truncation — larger variable OP smaller swizzle.
        // Instead of truncating the variable (which cascades mismatches downstream),
        // expand the swizzle to match the variable width by repeating its last char.
        // E.g. vec3_var - expr.xx → vec3_var - expr.xxx
        auto fixArithSwizzleExpand = [&result](const std::set<std::string>& large_vars,
                                               int                          var_width,
                                               int                          swizzle_width) {
            // One static forward+reverse pair per swizzle width (swizzle_width ∈ {2,3}).
            // Captures any \w+ identifier; C++ filter checks group1/group4 ∈ large_vars.
            static const std::regex re_fwd2(
                R"(\b(\w+)(\s*[+\-*/]\s*)(\w+)\.([xyzwrgbastpq]{2})\b)");
            static const std::regex re_rev2(
                R"((\w+)\.([xyzwrgbastpq]{2})(\s*[+\-*/]\s*)\b(\w+)\b)");
            static const std::regex re_fwd3(
                R"(\b(\w+)(\s*[+\-*/]\s*)(\w+)\.([xyzwrgbastpq]{3})\b)");
            static const std::regex re_rev3(
                R"((\w+)\.([xyzwrgbastpq]{3})(\s*[+\-*/]\s*)\b(\w+)\b)");

            const std::regex& re_fwd = (swizzle_width == 2) ? re_fwd2 : re_fwd3;
            const std::regex& re_rev = (swizzle_width == 2) ? re_rev2 : re_rev3;

            int  pad_count = var_width - swizzle_width;
            auto padSwizzle = [pad_count](const std::string& swiz) {
                std::string out  = swiz;
                char        last = out.back();
                for (int j = 0; j < pad_count; j++) out += last;
                return out;
            };

            // VAR OP WORD.XX — filter: group1 (VAR) must be in large_vars.
            // Skip-guard: if VAR is immediately followed by '.' in the original text,
            // the variable already has a swizzle suffix — return the match unchanged.
            {
                const std::string& textRef = result;
                regexTransformAll(result, re_fwd, [&](const std::smatch& m) -> std::string {
                    if (!large_vars.count(m[1].str())) return m[0].str();
                    size_t vEnd = (size_t)m.position() + m[1].length();
                    if (vEnd < textRef.size() && textRef[vEnd] == '.')
                        return m[0].str(); // skip: variable has swizzle
                    return m[1].str() + m[2].str() + m[3].str() + "." + padSwizzle(m[4].str());
                });
            }

            // Reverse: WORD.XX OP VAR — filter: group4 (VAR) must be in large_vars.
            // Skip-guard: same trailing-dot check on the VAR match end position.
            {
                const std::string& textRef = result;
                regexTransformAll(result, re_rev, [&](const std::smatch& m) -> std::string {
                    if (!large_vars.count(m[4].str())) return m[0].str();
                    size_t vStart = (size_t)m.position(4);
                    size_t vEnd   = vStart + m[4].length();
                    if (vEnd < textRef.size() && textRef[vEnd] == '.') return m[0].str(); // skip
                    return m[1].str() + "." + padSwizzle(m[2].str()) + m[3].str() + m[4].str();
                });
            }
        };
        const auto local_vec4 = collectLocal("vec4");
        // Expand runs for ALL names — including those shadowed at narrower
        // widths.  The Trunc pass above already skipped shadowed names, so
        // Expand and Trunc don't conflict.  For genuinely-shadowed names,
        // Expand handles the wider-scope case (e.g. hsv2rgb's vec3 p) and
        // the broadcast pass (further down) handles the narrower-scope case
        // (e.g. main's vec2 p).
        // vec3 var OP 2-component swizzle → expand swizzle to 3
        fixArithSwizzleExpand(local_vec3, 3, 2);
        // vec4 var OP 2-component swizzle → expand swizzle to 4
        fixArithSwizzleExpand(local_vec4, 4, 2);
        // vec4 var OP 3-component swizzle → expand swizzle to 4
        fixArithSwizzleExpand(local_vec4, 4, 3);
    }

    // Fix: HLSL implicit vec4→vec3 truncation in matrix*vector expressions.
    // WE mul(vec4(X), MAT) → ((MAT) * (vec4(X))) returns vec4 but may be assigned to vec3.
    // Add .xyz to truncate.
    {
        static const std::regex re(R"(\bvec3\s+(\w+)\s*=\s*(\([^;]*\)\s*\*\s*\(vec4\s*\([^;]*?\)\)\s*)\s*;)");
        result = std::regex_replace(result, re, "vec3 $1 = ($2).xyz;");
    }

    // Fix: HLSL pow(scalar, vecN) broadcasts the scalar; GLSL requires matching genType.
    // When the pow() result is used inside a vecN() constructor, move the broadcast inside:
    //   vecN(pow(X, Y)) → pow(vecN(X), vecN(Y))
    // Wrapping an already-vecN arg in vecN() is a safe copy-constructor identity.
    // Only handles one level of nesting inside each pow argument (sufficient in practice).
    {
        static const std::regex re(R"(\b(vec[234])\s*\(\s*pow\s*\()"
                      R"(([^(),]*(?:\([^)]*\)[^(),]*)*),\s*)"
                      R"(([^()]*(?:\([^)]*\)[^()]*)*)\)\s*\))");
        result = std::regex_replace(result, re, "pow($1($2), $1($3))");
    }

    // Fix: "float VAR = texture(SAMPLER, COORD);" → add .x swizzle
    // HLSL implicitly converts vec4 texture result to float (first component);
    // GLSL requires an explicit swizzle.
    {
        static const std::regex re(R"(\bfloat\s+(\w+)\s*=\s*(texture\w*\s*\([^;]*?\))\s*;)");
        result = std::regex_replace(result, re, "float $1 = $2.x;");
    }

    // Fix: vec4 texture variable used in scalar arithmetic → add .x swizzle at use site.
    // HLSL: float4 timer = tex2D(...); float off = u_scale * timer;  — implicit truncation.
    // GLSL: vec4 timer = texture(...);  u_scale * timer → vec4, not float.
    // Detect "vec4 VAR = texture(...)" vars whose bare name appears in float*VAR or VAR*float
    // expressions (no existing swizzle), and append .x at those use sites.
    {
        // Collect vec4 vars assigned from texture()
        std::set<std::string> tex_vec4_vars;
        static const std::regex            re_decl(R"(\bvec4\s+(\w+)\s*=\s*texture\w*\s*\()");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_decl);
             it != std::sregex_iterator();
             ++it)
            tex_vec4_vars.insert((*it)[1].str());
        // Collect known float uniforms
        std::set<std::string> float_vars;
        static const std::regex            re_float(R"(\buniform\s+float\s+(\w+)\s*;)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_float);
             it != std::sregex_iterator();
             ++it)
            float_vars.insert((*it)[1].str());
        // Also add local float vars
        static const std::regex re_flocal(R"(\bfloat\s+(\w+)\s*=)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_flocal);
             it != std::sregex_iterator();
             ++it)
            float_vars.insert((*it)[1].str());
        // Detect which tex_vec4_vars are used as scalars in float*VAR or VAR*float
        // arithmetic — one constant pattern replaces the O(tex×float) per-pair NFA loop.
        // Lookahead on group2 ensures the second operand has no existing swizzle/subscript.
        // The first operand can't have one either: \b(\w+) stops at '.' so "tvar.x * fvar"
        // would leave \s*\*\s* unmatched after "tvar".
        static const std::regex re_arith_any(R"(\b(\w+)\s*\*\s*(\w+)(?!\s*[.\[]))");
        std::set<std::string>   scalar_tvars;
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_arith_any);
             it != std::sregex_iterator();
             ++it) {
            const std::string& g1 = (*it)[1].str();
            const std::string& g2 = (*it)[2].str();
            if (float_vars.count(g1) && tex_vec4_vars.count(g2))
                scalar_tvars.insert(g2);
            if (tex_vec4_vars.count(g1) && float_vars.count(g2))
                scalar_tvars.insert(g1);
        }
        // String find+replace helper — mark strings contain only alphanumeric and
        // underscores (no regex metacharacters), so plain string ops are safe and
        // faster than constructing a new NFA per tvar.
        auto strReplaceAll = [](std::string& s, const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        // Constant bare-use pattern — captures any word-boundary identifier not followed
        // by '.', '[', or another word char.  Equivalent to the old per-tvar
        // \b{tvar}(?!\s*[.\[\w]) after filtering on m[1] == tvar.
        static const std::regex re_bare_use(R"(\b(\w+)(?!\s*[.\[\w]))");
        for (const auto& tvar : scalar_tvars) {
            // Replace bare uses of tvar (not followed by . or [ or word char) with tvar.x
            // but preserve declarations: "vec4 tvar = texture" and "in vec4 tvar;"
            // Mark-restore protocol: rename the declaration spans FIRST so the bare-use
            // pass cannot corrupt them, then restore after bare-use replacement completes.
            std::string mark = "__DECL_MARK_" + tvar;
            // Mark local declaration (plain string — faster, no NFA compile per tvar)
            strReplaceAll(result, "vec4 " + tvar + " = texture", "vec4 " + mark + " = texture");
            // Mark 'in' declaration
            strReplaceAll(result, "in vec4 " + tvar + ";", "in vec4 " + mark + ";");
            // Replace bare uses with .x (constant pattern + C++ name filter)
            regexTransformAll(result, re_bare_use, [&](const std::smatch& m) -> std::string {
                if (m[1].str() != tvar) return m[0].str();
                return tvar + ".x";
            });
            // Restore marks (plain string — mark has no regex metacharacters)
            strReplaceAll(result, mark, tvar);
        }
    }

    // Fix: integer literal as ternary condition → bool()
    // HLSL allows int in ternary condition; GLSL requires bool.
    // Match bare integer after = ( , that is followed by ? (ternary operator).
    {
        static const std::regex re(R"(([\=\(,]\s*)(\d+)(\s*\?))");
        result = std::regex_replace(result, re, "$1bool($2)$3");
    }

    // Fix: float identifier as ternary condition → bool()
    // HLSL accepts any nonzero numeric in ternary cond; GLSL requires bool.
    // Driver: The Blur (3562086244) ships
    //   float outside = 1.0; … vec4 final = outside ? inSmooth : outSmooth;
    {
        std::set<std::string> float_locals_for_ternary;
        static const std::regex            re_floc(R"(\bfloat\s+(\w+)\s*[=;,)])");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_floc);
             it != std::sregex_iterator(); ++it)
            float_locals_for_ternary.insert((*it)[1].str());
        for (const auto& f : float_locals_for_ternary) {
            // Match `<leading_punct> <whitespace> f <whitespace> ?` —
            // leading punct rules out `foo.f`, `myF` etc.  The same f could
            // appear in other contexts (e.g. `a.f`, `*f`); we only catch
            // ternary-cond shape.
            std::regex re_t(R"(([\=\(,;]\s*))" + f + R"((\s*\?))");
            result = std::regex_replace(result, re_t, "$1bool(" + f + ")$2");
        }
    }

    // Fix: if (<float-expression>) → if (bool(<float-expression>))
    // HLSL accepts any nonzero numeric in `if`/`while`; GLSL requires bool.
    // Heuristic: wrap when the top-level expression contains NO comparison
    // (<, >, ==, !=, <=, >=) and NO logical operator (&&, ||) — those already
    // produce a bool.  Already-wrapped `if (bool(...))` is left alone.
    // Driver: Game Of Life (3453251764) canvas chain — `if(u_mouseDown.x *
    // NOT(u_mouseDown.y))`, `if(u_enablePreview)`, `if(useLastAsNewUndoFrame)`.
    {
        std::string out;
        out.reserve(result.size());
        size_t i = 0;
        while (i < result.size()) {
            bool word_boundary = (i == 0) ||
                                 (! std::isalnum((unsigned char)result[i - 1]) &&
                                  result[i - 1] != '_' && result[i - 1] != '#');
            bool keyword_if = word_boundary && i + 2 <= result.size() &&
                              result[i] == 'i' && result[i + 1] == 'f' &&
                              (i + 2 == result.size() ||
                               (! std::isalnum((unsigned char)result[i + 2]) &&
                                result[i + 2] != '_'));
            if (! keyword_if) {
                out.push_back(result[i++]);
                continue;
            }
            size_t j = i + 2;
            while (j < result.size() && std::isspace((unsigned char)result[j])) ++j;
            if (j >= result.size() || result[j] != '(') {
                out.push_back(result[i++]);
                continue;
            }
            size_t close = findMatchingParen(result, j);
            if (close == std::string::npos || close <= j + 1) {
                out.push_back(result[i++]);
                continue;
            }
            // Inner range is (j+1, close-1) since close is one PAST ')'
            size_t innerBeg = j + 1;
            size_t innerEnd = close - 1; // exclusive: position of ')'
            // Trim whitespace
            while (innerBeg < innerEnd && std::isspace((unsigned char)result[innerBeg]))
                ++innerBeg;
            while (innerEnd > innerBeg && std::isspace((unsigned char)result[innerEnd - 1]))
                --innerEnd;
            if (innerBeg >= innerEnd) {
                out.push_back(result[i++]);
                continue;
            }
            // Already wrapped: starts with "bool(" and the matching paren
            // closes at innerEnd.
            bool already_bool = false;
            if (innerEnd - innerBeg >= 6 && result.compare(innerBeg, 5, "bool(") == 0) {
                size_t inner_close = findMatchingParen(result, innerBeg + 4);
                if (inner_close == innerEnd) already_bool = true;
            }
            // Top-level boolean operator scan
            auto has_top_level_bool_op = [&]() -> bool {
                int depth = 0;
                for (size_t k = innerBeg; k < innerEnd; ++k) {
                    char c = result[k];
                    if (c == '(')
                        ++depth;
                    else if (c == ')')
                        --depth;
                    else if (depth == 0) {
                        if (c == '<' || c == '>') return true;
                        if (k + 1 < innerEnd) {
                            char n = result[k + 1];
                            if ((c == '=' && n == '=') || (c == '!' && n == '=')) return true;
                            if ((c == '&' && n == '&') || (c == '|' && n == '|')) return true;
                        }
                    }
                }
                return false;
            };
            if (already_bool || has_top_level_bool_op()) {
                out.push_back(result[i++]);
                continue;
            }
            // Emit:  "if (bool(" + inner + "))" — preserve the user's spacing
            // between `if` and `(` so output reads naturally.
            out.append(result, i, j - i + 1); // "if (" up to and including '('
            out.append("bool(");
            out.append(result, innerBeg, innerEnd - innerBeg);
            out.append(")");
            out.push_back(')');
            i = close;
        }
        result = std::move(out);
    }

    // Fix: mix(<vec>, <vec>, <bool>) → mix(<vec>, <vec>, float(<bool>))
    // HLSL `lerp(a, b, bool)` implicitly converts; GLSL has no scalar-bool
    // overload — only the per-component `bvec` form, which would still need
    // explicit construction.  Wrap with float() instead.
    // Driver: Game Of Life canvas_paint.frag —
    //   `mix(mask, vec2(mask.r, 1.-mask.g), isMode(u_command, CMD_STEP_BLUEPRINT))`
    // where `isMode` returns `bool`.
    {
        // Collect user-defined bool-returning functions and bool locals.
        std::set<std::string> bool_fns;
        static const std::regex            re_fn(R"(\bbool\s+(\w+)\s*\([^)]*\)\s*\{)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_fn);
             it != std::sregex_iterator();
             ++it)
            bool_fns.insert((*it)[1].str());
        std::set<std::string> bool_vars;
        static const std::regex            re_bv(R"(\bbool\s+(\w+)\s*[=;,)])");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_bv);
             it != std::sregex_iterator();
             ++it)
            bool_vars.insert((*it)[1].str());

        std::string out;
        out.reserve(result.size());
        size_t i = 0;
        while (i < result.size()) {
            bool word_boundary = (i == 0) ||
                                 (! std::isalnum((unsigned char)result[i - 1]) &&
                                  result[i - 1] != '_');
            if (! (word_boundary && i + 3 <= result.size() &&
                   result.compare(i, 3, "mix") == 0 &&
                   (i + 3 == result.size() ||
                    (! std::isalnum((unsigned char)result[i + 3]) &&
                     result[i + 3] != '_')))) {
                out.push_back(result[i++]);
                continue;
            }
            size_t j = i + 3;
            while (j < result.size() && std::isspace((unsigned char)result[j])) ++j;
            if (j >= result.size() || result[j] != '(') {
                out.push_back(result[i++]);
                continue;
            }
            size_t close = findMatchingParen(result, j);
            if (close == std::string::npos) {
                out.push_back(result[i++]);
                continue;
            }
            std::vector<std::pair<size_t, size_t>> args;
            int                                    depth     = 0;
            size_t                                 arg_start = j + 1;
            for (size_t k = j + 1; k + 1 < close; ++k) {
                char c = result[k];
                if (c == '(')
                    ++depth;
                else if (c == ')')
                    --depth;
                else if (c == ',' && depth == 0) {
                    args.push_back({ arg_start, k });
                    arg_start = k + 1;
                }
            }
            args.push_back({ arg_start, close - 1 });
            if (args.size() != 3) {
                out.push_back(result[i++]);
                continue;
            }
            // Inspect arg2 (third arg)
            size_t a2s = args[2].first;
            size_t a2e = args[2].second;
            while (a2s < a2e && std::isspace((unsigned char)result[a2s])) ++a2s;
            while (a2e > a2s && std::isspace((unsigned char)result[a2e - 1])) --a2e;
            std::string a2 = (a2e > a2s) ? result.substr(a2s, a2e - a2s) : "";
            bool        is_bool = false;
            if (! a2.empty()) {
                // Bare identifier matching a known bool local
                if (std::regex_match(a2, std::regex(R"(\w+)")) && bool_vars.count(a2))
                    is_bool = true;
                // Call to a known bool-returning function: IDENT(...)
                if (! is_bool) {
                    std::smatch m;
                    if (std::regex_match(a2, m, std::regex(R"((\w+)\s*\([\s\S]*\))"))) {
                        if (bool_fns.count(m[1].str())) is_bool = true;
                    }
                }
                // Already wrapped with float() — leave alone
                if (a2.compare(0, 6, "float(") == 0) is_bool = false;
            }
            if (! is_bool) {
                out.push_back(result[i++]);
                continue;
            }
            // Emit: "mix(<arg0>,<arg1>, float(<arg2>))"
            out.append("mix(");
            out.append(result, args[0].first, args[0].second - args[0].first);
            out.append(",");
            out.append(result, args[1].first, args[1].second - args[1].first);
            out.append(", float(");
            out.append(a2);
            out.append("))");
            i = close;
        }
        result = std::move(out);
    }

    // Fix: mix(vecN(...), <scalar_float>, ...) with mismatched ranks.
    // HLSL's lerp() broadcasts scalar→vector; GLSL's mix requires matching ranks.
    // The right rewrite depends on the assignment LHS type:
    //   * LHS is `float`        → truncate first arg to scalar: `vecN(...).x`
    //                             (matches WE's HLSL→GLSL behavior where the
    //                              vec result is truncated on assign to float).
    //   * LHS is `vec[234]` (matching N) → broadcast scalar: `vecN(scalar)`.
    //   * Unknown LHS           → leave alone (conservative).
    // Driver: Game Of Life (3453251764) canvas_pen_influence.frag —
    // `float influence = ...; influence = mix(vec2(0.,1.), influence, sameUiPart);`
    {
        // Collect known typed identifiers: locals, function params, uniforms.
        std::set<std::string> float_idents;
        std::set<std::string> vec_idents; // any vec2/vec3/vec4
        static const std::regex            re_float(R"(\bfloat\s+(\w+)\s*[=;,)])");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_float);
             it != std::sregex_iterator();
             ++it)
            float_idents.insert((*it)[1].str());
        static const std::regex re_uniform_float(R"(\buniform\s+float\s+(\w+))");
        for (auto it =
                 std::sregex_iterator(result.begin(), result.end(), re_uniform_float);
             it != std::sregex_iterator();
             ++it)
            float_idents.insert((*it)[1].str());
        static const std::regex re_vec(R"(\bvec[234]\s+(\w+)\s*[=;,)])");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_vec);
             it != std::sregex_iterator();
             ++it)
            vec_idents.insert((*it)[1].str());
        static const std::regex re_uniform_vec(R"(\buniform\s+vec[234]\s+(\w+))");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_uniform_vec);
             it != std::sregex_iterator();
             ++it)
            vec_idents.insert((*it)[1].str());

        std::string out;
        out.reserve(result.size());
        size_t i = 0;
        while (i < result.size()) {
            bool word_boundary = (i == 0) ||
                                 (! std::isalnum((unsigned char)result[i - 1]) &&
                                  result[i - 1] != '_');
            if (! (word_boundary && i + 3 <= result.size() &&
                   result.compare(i, 3, "mix") == 0 &&
                   (i + 3 == result.size() ||
                    (! std::isalnum((unsigned char)result[i + 3]) &&
                     result[i + 3] != '_')))) {
                out.push_back(result[i++]);
                continue;
            }
            size_t j = i + 3;
            while (j < result.size() && std::isspace((unsigned char)result[j])) ++j;
            if (j >= result.size() || result[j] != '(') {
                out.push_back(result[i++]);
                continue;
            }
            size_t close = findMatchingParen(result, j);
            if (close == std::string::npos) {
                out.push_back(result[i++]);
                continue;
            }
            // Split into 3 args at top-level commas
            std::vector<std::pair<size_t, size_t>> args;
            int                                    depth     = 0;
            size_t                                 arg_start = j + 1;
            for (size_t k = j + 1; k + 1 < close; ++k) {
                char c = result[k];
                if (c == '(')
                    ++depth;
                else if (c == ')')
                    --depth;
                else if (c == ',' && depth == 0) {
                    args.push_back({ arg_start, k });
                    arg_start = k + 1;
                }
            }
            args.push_back({ arg_start, close - 1 });
            if (args.size() != 3) {
                out.push_back(result[i++]);
                continue;
            }
            // Detect arg0's vector rank N from one of these shapes:
            //   * `vec[234](...)` constructor.
            //   * Bare named identifier whose declared type is `uniform/in/out
            //     vec[234]` or local `vec[234] NAME`.  Driver: 2k+ Retro
            //     Cyber Sunset (1117550117) and others share workshop effect
            //     2079712247 reflection.frag:
            //       glOutColor = vec4(mix(mix(albedo, reflected.x, mask), ...))
            //     where `albedo` is a local `vec4 albedo = texture(...)`.
            //     glslang rejects `mix(vec4, float, float)` with
            //     "'mix' : no matching overloaded function found".
            size_t a0s = args[0].first;
            size_t a0e = args[0].second;
            while (a0s < a0e && std::isspace((unsigned char)result[a0s])) ++a0s;
            // Trim trailing whitespace too so the named-identifier match is exact.
            size_t a0e_trim = a0e;
            while (a0e_trim > a0s && std::isspace((unsigned char)result[a0e_trim - 1]))
                --a0e_trim;
            char N = 0;
            if (a0e - a0s >= 5 && result.compare(a0s, 3, "vec") == 0 &&
                (result[a0s + 3] == '2' || result[a0s + 3] == '3' ||
                 result[a0s + 3] == '4')) {
                size_t p = a0s + 4;
                while (p < a0e && std::isspace((unsigned char)result[p])) ++p;
                if (p < a0e && result[p] == '(') N = result[a0s + 3];
            }
            if (! N && a0e_trim > a0s) {
                // Try named-identifier match.  Identifier characters only —
                // anything else (`.`, `[`, math ops, parens) means the arg is
                // a sub-expression we can't classify cheaply; bail.
                bool plain = true;
                for (size_t p = a0s; p < a0e_trim; ++p) {
                    char c = result[p];
                    if (! (std::isalnum((unsigned char)c) || c == '_')) {
                        plain = false;
                        break;
                    }
                }
                if (plain) {
                    std::string nm(result, a0s, a0e_trim - a0s);
                    // vec_idents tracks vec2/3/4; we need the specific rank.
                    static const std::regex re_vec_named_n(
                        R"(\b(?:uniform|in|out|varying)?\s*vec([234])\s+(\w+))");
                    auto it_n = std::sregex_iterator(result.begin(), result.end(),
                                                     re_vec_named_n);
                    for (; it_n != std::sregex_iterator(); ++it_n) {
                        if ((*it_n)[2].str() == nm) { N = (*it_n)[1].str()[0]; break; }
                    }
                }
            }
            if (! N) {
                out.push_back(result[i++]);
                continue;
            }
            // Trim arg1.  Accept either:
            //   * A single identifier in float_idents (the original case).
            //   * `<known_vec>.<single_component>` access — a vec-projection
            //     that yields a scalar.  Driver: workshop reflection
            //     effect 2079712247 used by 2k+ Retro Cyber Sunset
            //     (1117550117), Perfect View (1235913324), Moon (2157202681)
            //     etc. — writes `mix(albedo, reflected.x, mask)` where
            //     `albedo` is local vec4 and `reflected` is local vec4.
            size_t a1s = args[1].first;
            size_t a1e = args[1].second;
            while (a1s < a1e && std::isspace((unsigned char)result[a1s])) ++a1s;
            while (a1e > a1s && std::isspace((unsigned char)result[a1e - 1])) --a1e;
            std::string a1_str;
            if (a1e > a1s) a1_str.assign(result, a1s, a1e - a1s);
            bool a1_is_scalar = false;
            std::string a1_emit = a1_str;
            if (! a1_str.empty()) {
                // Plain identifier?
                bool plain_id = true;
                for (char c : a1_str) {
                    if (! std::isalnum((unsigned char)c) && c != '_') { plain_id = false; break; }
                }
                if (plain_id && float_idents.count(a1_str)) {
                    a1_is_scalar = true;
                } else {
                    // ident.<single component>
                    auto dot = a1_str.find('.');
                    if (dot != std::string::npos && dot + 2 == a1_str.size()) {
                        char comp = a1_str[dot + 1];
                        if (comp == 'x' || comp == 'y' || comp == 'z' || comp == 'w' ||
                            comp == 'r' || comp == 'g' || comp == 'b' || comp == 'a') {
                            std::string base(a1_str, 0, dot);
                            bool plain_base = ! base.empty();
                            for (char c : base) {
                                if (! std::isalnum((unsigned char)c) && c != '_') {
                                    plain_base = false; break;
                                }
                            }
                            if (plain_base &&
                                (vec_idents.count(base) || float_idents.count(base))) {
                                a1_is_scalar = true;
                            }
                        }
                    }
                }
            }
            std::string a1_name = a1_emit;
            if (! a1_is_scalar) {
                out.push_back(result[i++]);
                continue;
            }
            // Look backward over whitespace to find an assignment of the
            // form  IDENT = mix(...)  or  IDENT = ... mix(...)
            // We accept it only when an `=` (single) sits between mix and an
            // identifier, with no other tokens — i.e. the simple case
            // `LHS = mix(...)`.
            auto lhsType = [&]() -> char { // 'f' float, 'v' vec, '2'/'3'/'4' rank when known vec, 0 unknown
                ssize_t k = (ssize_t)i - 1;
                while (k >= 0 && (result[k] == ' ' || result[k] == '\t')) --k;
                if (k < 0 || result[k] != '=') return 0;
                // Reject ==, !=, <=, >= (any of these can precede mix in an
                // expression context where mix is not directly assigned).
                if (k > 0 && (result[k - 1] == '=' || result[k - 1] == '!' ||
                              result[k - 1] == '<' || result[k - 1] == '>'))
                    return 0;
                --k;
                while (k >= 0 && std::isspace((unsigned char)result[k])) --k;
                ssize_t id_end = k + 1;
                while (k >= 0 &&
                       (std::isalnum((unsigned char)result[k]) || result[k] == '_'))
                    --k;
                ssize_t id_beg = k + 1;
                if (id_end <= id_beg) return 0;
                std::string ident = result.substr((size_t)id_beg, (size_t)(id_end - id_beg));
                if (float_idents.count(ident) && ! vec_idents.count(ident)) return 'f';
                if (vec_idents.count(ident)) return 'v';
                return 0;
            }();
            std::string Ns(1, N);
            if (lhsType == 'f') {
                // LHS is float — truncate first arg to scalar.
                // Emit: "mix(<arg0>.x,<arg1>,<arg2>)"
                out.append("mix(");
                out.append(result, args[0].first, args[0].second - args[0].first);
                out.append(".x,");
                out.append(result, args[1].first, args[1].second - args[1].first);
                out.append(",");
                out.append(result, args[2].first, args[2].second - args[2].first);
                out.push_back(')');
                i = close;
            } else if (lhsType == 'v') {
                // LHS is vec — broadcast scalar to vecN.
                // Emit: "mix(<arg0>, vecN(<arg1>),<arg2>)"
                out.append("mix(");
                out.append(result, args[0].first, args[0].second - args[0].first);
                out.append(", vec");
                out.append(Ns);
                out.append("(");
                out.append(a1_name);
                out.append("),");
                out.append(result, args[2].first, args[2].second - args[2].first);
                out.push_back(')');
                i = close;
            } else {
                // Unknown LHS context.  When arg0 is a known vecN AND arg1 is a
                // known float identifier, HLSL's `lerp` would broadcast arg1
                // up to vecN.  Default to that behavior — it matches the
                // common nested-mix pattern (e.g. `vec4(mix(mix(albedo,
                // reflected.x, mask), …))` in workshop reflection effects)
                // and never loses information vs. the rejected
                // "no matching overloaded mix" alternative.
                out.append("mix(");
                out.append(result, args[0].first, args[0].second - args[0].first);
                out.append(", vec");
                out.append(Ns);
                out.append("(");
                out.append(a1_name);
                out.append("),");
                out.append(result, args[2].first, args[2].second - args[2].first);
                out.push_back(')');
                i = close;
            }
        }
        result = std::move(out);
    }

    // Fix: mix(<scalar_ident>, <vec_ident>, <scalar>) — broadcast arg0 to
    // match arg1's vector rank.  HLSL's lerp() broadcasts scalar→vec; GLSL's
    // mix expects matching ranks for arg0 and arg1.
    //
    // Driver: jake. (3353695150) workshop effect ships
    //   vec3 vibrance(vec3 color, float luma) {
    //       …
    //       return mix(luma, color, (1.0 + (u_vibrance * (…))));
    //   }
    // luma is float, color is vec3 → arg0/arg1 rank mismatch.  Wrap as
    // `mix(vec3(luma), color, …)`.
    {
        // Re-collect identifier types (same source might have been edited
        // since the earlier mix pass).
        std::set<std::string> float_idents2;
        std::map<std::string, int> vec_idents2;
        {
            static const std::regex re_float(R"(\b(?:float|in\s+float|uniform\s+float)\s+(\w+)\s*[=;,)])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_float);
                 it != std::sregex_iterator(); ++it)
                float_idents2.insert((*it)[1].str());
            static const std::regex re_vecn(
                R"(\b(?:uniform|in|out|varying)?\s*vec([234])\s+(\w+)\s*[=;,)])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_vecn);
                 it != std::sregex_iterator(); ++it)
                vec_idents2[(*it)[2].str()] = (*it)[1].str()[0] - '0';
        }
        // Match `mix(<ident>, <ident>, ...)` exactly — both args are
        // single identifiers, comma-separated at depth 0.
        std::string out2;
        out2.reserve(result.size());
        size_t i2 = 0;
        while (i2 < result.size()) {
            bool word_boundary2 =
                (i2 == 0) || (! std::isalnum((unsigned char)result[i2 - 1]) &&
                              result[i2 - 1] != '_');
            if (! (word_boundary2 && i2 + 3 <= result.size() &&
                   result.compare(i2, 3, "mix") == 0 &&
                   (i2 + 3 == result.size() ||
                    (! std::isalnum((unsigned char)result[i2 + 3]) &&
                     result[i2 + 3] != '_')))) {
                out2.push_back(result[i2++]);
                continue;
            }
            size_t j = i2 + 3;
            while (j < result.size() && std::isspace((unsigned char)result[j])) ++j;
            if (j >= result.size() || result[j] != '(') {
                out2.push_back(result[i2++]);
                continue;
            }
            size_t close = findMatchingParen(result, j);
            if (close == std::string::npos) {
                out2.push_back(result[i2++]);
                continue;
            }
            std::vector<std::pair<size_t, size_t>> a;
            int                                    d  = 0;
            size_t                                 sa = j + 1;
            for (size_t k = j + 1; k + 1 < close; ++k) {
                char c = result[k];
                if (c == '(' || c == '[') ++d;
                else if (c == ')' || c == ']') --d;
                else if (c == ',' && d == 0) { a.push_back({ sa, k }); sa = k + 1; }
            }
            a.push_back({ sa, close - 1 });
            if (a.size() != 3) { out2.push_back(result[i2++]); continue; }

            auto trimid = [&](size_t s, size_t e) {
                while (s < e && std::isspace((unsigned char)result[s])) ++s;
                while (e > s && std::isspace((unsigned char)result[e - 1])) --e;
                std::string id;
                bool        plain = (e > s);
                for (size_t k = s; k < e && plain; ++k) {
                    char c = result[k];
                    if (! (std::isalnum((unsigned char)c) || c == '_')) plain = false;
                }
                return plain ? std::string(result, s, e - s) : std::string();
            };
            std::string a0 = trimid(a[0].first, a[0].second);
            std::string a1 = trimid(a[1].first, a[1].second);
            auto vit = vec_idents2.find(a1);
            if (a0.empty() || a1.empty() || ! float_idents2.count(a0) ||
                vit == vec_idents2.end()) {
                out2.push_back(result[i2++]);
                continue;
            }
            // Wrap arg0 in vecN(...) matching arg1's rank.
            std::string Ns(1, char('0' + vit->second));
            out2.append("mix(vec").append(Ns).append("(").append(a0).append(")");
            out2.append(",");
            out2.append(result, a[1].first, a[1].second - a[1].first);
            out2.append(",");
            out2.append(result, a[2].first, a[2].second - a[2].first);
            out2.push_back(')');
            i2 = close;
        }
        result = std::move(out2);
    }

    // Fix: GLSL builtins reject `int` arg when overload expects `float`.
    // HLSL silently promotes; GLSL doesn't.  Driver: Cybering (2326102392)
    // ships `step(0, uv) * step(uv, 1)` — `0` and `1` are integer literals,
    // `uv` is vec2.  glslang reports "step: no matching overloaded function".
    // Rewrite bare integer literals adjacent to `,` or `(` inside the args of
    // these builtins to float form (`0` → `0.0`, `1` → `1.0`).
    {
        // Match `<builtin>(<args>)` and convert `<int_literal>` arg tokens.
        // Recursive: descends into nested calls so cases like
        //   step(0.99, _wedot(step(0, uv) * step(uv, 1), …))
        // also promote the inner `step(0,…)`/`step(…,1)` literals.
        static const std::set<std::string> int_to_float_builtins = {
            "step", "smoothstep", "mix", "clamp", "min", "max",
            "_wemn", "_wemx", "pow", "_wep", "atan", "mod"
        };
        // Try to promote `result[arg_pos..end)` if it's an int-literal token.
        // On success, emit the promoted form and advance `arg_pos`.
        auto try_promote = [&](const std::string& src, size_t& arg_pos, size_t end,
                               std::string& out_buf) -> bool {
            size_t p = arg_pos;
            while (p < end && std::isspace((unsigned char)src[p])) ++p;
            size_t sign_end = p;
            if (p < end && (src[p] == '+' || src[p] == '-')) ++p;
            while (p < end && std::isspace((unsigned char)src[p])) ++p;
            size_t num_start = p;
            while (p < end && std::isdigit((unsigned char)src[p])) ++p;
            if (p == num_start) return false;
            char next = (p < end) ? src[p] : ')';
            bool is_int = (next != '.' && next != 'e' && next != 'E' &&
                           next != 'x' && next != 'X' && next != 'b' && next != 'B');
            if (! (is_int && (next == ',' || next == ')' ||
                              std::isspace((unsigned char)next) ||
                              next == '+' || next == '-' || next == '*' || next == '/')))
                return false;
            // Also reject if the digits are preceded by `_` or alnum (would be
            // part of an identifier).  We've already walked back through
            // whitespace and at most one sign char, so check `sign_end - 1`.
            if (sign_end > arg_pos) {
                // We have leading whitespace at arg_pos..sign_end-1 — no issue.
            }
            for (size_t q = arg_pos; q < p; ++q) out_buf.push_back(src[q]);
            out_buf.append(".0");
            arg_pos = p;
            return true;
        };
        std::function<std::string(const std::string&)> promote;
        promote = [&](const std::string& src) -> std::string {
            std::string out;
            out.reserve(src.size() + 32);
            size_t i = 0;
            while (i < src.size()) {
                bool word_start = (i == 0) || (! std::isalnum((unsigned char)src[i - 1]) &&
                                                src[i - 1] != '_');
                if (! word_start) { out.push_back(src[i++]); continue; }
                // Read identifier
                size_t j = i;
                while (j < src.size() &&
                       (std::isalnum((unsigned char)src[j]) || src[j] == '_')) ++j;
                if (j == i) { out.push_back(src[i++]); continue; }
                std::string id(src, i, j - i);
                // Skip whitespace, expect `(`
                size_t k = j;
                while (k < src.size() && std::isspace((unsigned char)src[k])) ++k;
                if (k >= src.size() || src[k] != '(' ||
                    ! int_to_float_builtins.count(id)) {
                    // Not our function — emit identifier as-is.  Do NOT skip
                    // past it; the outer loop will pick up at `j`.
                    out.append(src, i, j - i);
                    i = j;
                    continue;
                }
                size_t close = findMatchingParen(src, k);
                if (close == std::string::npos) {
                    out.append(src, i, j - i);
                    i = j;
                    continue;
                }
                // Recurse on the args so nested builtin calls are promoted too.
                std::string inner = promote(std::string(src, k + 1, (close - 1) - (k + 1)));
                // Emit identifier + `(`
                out.append(src, i, j - i);
                for (size_t w = j; w <= k; ++w) out.push_back(src[w]);
                // Walk `inner` and promote bare int literals at depth 0.
                size_t arg_pos = 0;
                int    depth   = 0;
                // First arg has no preceding separator — check up front.
                try_promote(inner, arg_pos, inner.size(), out);
                while (arg_pos < inner.size()) {
                    char c = inner[arg_pos];
                    if (c == '(') ++depth;
                    else if (c == ')') --depth;
                    if (depth == 0 && (c == ',' || c == '(' ||
                                       std::isspace((unsigned char)c) ||
                                       c == '+' || c == '-' || c == '*' || c == '/')) {
                        // Exponent-sign guard: `1e-6` must not become `1e-6.0`.
                        // If `+/-` is preceded by `e/E` after a digit, this is
                        // part of a float literal — emit it and skip promote.
                        bool exponent_sign = false;
                        if ((c == '+' || c == '-') && arg_pos > 0) {
                            char prev = inner[arg_pos - 1];
                            if (prev == 'e' || prev == 'E') {
                                if (arg_pos > 1 &&
                                    std::isdigit((unsigned char)inner[arg_pos - 2]))
                                    exponent_sign = true;
                            }
                        }
                        out.push_back(c);
                        ++arg_pos;
                        if (! exponent_sign)
                            try_promote(inner, arg_pos, inner.size(), out);
                        continue;
                    }
                    out.push_back(c);
                    ++arg_pos;
                }
                out.push_back(')');
                i = close;
            }
            return out;
        };
        result = promote(result);
    }

    // Fix: `vecN VAR = mix(<numeric_literal>, …, …);` — LHS-aware arg0
    // broadcast.  Driver: Retro Island (2270319656) ships
    //   vec3 shadows = mix(0.5, c_Stint * (…), 1.0 - t);
    // HLSL broadcasts the `0.5` literal up to vec3; GLSL errors with "mix:
    // no matching overloaded".  Wrap arg0 in `vecN(literal)` matching LHS.
    {
        static const std::regex re(
            R"(\b(vec[234])(\s+\w+\s*=\s*mix\s*\()(\s*\d+(?:\.\d*)?(?:e[-+]?\d+)?\s*),)");
        result = std::regex_replace(result, re, "$1$2$1($3),");
    }

    // Fix: `mix(<float_ident>, vec[234](<float_ident>), <scalar>)` — unwrap
    // when BOTH args 0 and 1's inner are float scalars in the same context.
    // Driver: The Blur (3562086244) ships
    //   float BlendTransparency(float base, float blend, float opacity) {
    //       …
    //       return mix(base, vec3(transparency), opacity);
    //   }
    // Both `base` and `transparency` are float params.  The vec3(...) wrap
    // around `transparency` is a wallpaper-author artifact; unwrap to
    // `mix(base, transparency, opacity)` (all float).
    //
    // Only fire when BOTH arg0 and arg1's inner identifier are known floats
    // (so we don't accidentally unwrap `mix(vec3_col, vec3(scalar), t)`
    // which the broadcast pass deliberately produces).
    {
        std::set<std::string> float_idents_u;
        static const std::regex re_fid(R"(\b(?:in\s+|out\s+|uniform\s+)?float\s+(\w+)\s*[=;,)])");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_fid);
             it != std::sregex_iterator(); ++it)
            float_idents_u.insert((*it)[1].str());

        static const std::regex re(R"(\bmix\s*\(\s*(\w+)\s*,\s*vec[234]\s*\(\s*(\w+)\s*\)\s*,)");
        std::string out_u;
        out_u.reserve(result.size());
        size_t lastPos = 0;
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
             it != std::sregex_iterator(); ++it) {
            std::string a0 = (*it)[1].str();
            std::string a1 = (*it)[2].str();
            if (! float_idents_u.count(a0) || ! float_idents_u.count(a1)) continue;
            out_u.append(result, lastPos, it->position() - lastPos);
            out_u.append("mix(").append(a0).append(", ").append(a1).append(",");
            lastPos = it->position() + it->length();
        }
        out_u.append(result, lastPos, std::string::npos);
        result = std::move(out_u);
    }

    // Fix: (expr CMP expr) in arithmetic → float(expr CMP expr)
    // HLSL allows bool in arithmetic (true=1.0, false=0.0); GLSL does not.
    // Matches parenthesized comparison followed by arithmetic operator.
    //
    // Pre-step: ensure a space between control-flow keywords and an
    // open-paren.  glslang's preprocessor sometimes drops the space —
    // Mikey Tokyo Revengers (2622312893) ships `return (f >=0)` in source
    // and the preprocessor output reads `return(f >= 0)`.  Without the
    // space, the leading-context guard below either rejects the wrap
    // (leaving the bool-in-arith error) or — historically — glued
    // `float` onto `return`, emitting `returnfloat(f >= 0)`.
    //
    // The leading-context guard `[\s,({=+\-*/]` then ensures we only
    // wrap stand-alone parenthesized comparisons, not function-call
    // argument lists.  Without the guard, `foo(x > 0) * y` would have
    // been mis-transformed into `foofloat(x > 0) * y`.
    {
        static const std::regex re_kw(R"(\b(return|if|while|for|do|switch)\()");
        result = std::regex_replace(result, re_kw, "$1 (");
    }
    {
        static const std::regex re(R"((^|[\s,({=+\-*/])\(([^()]*(?:<=|>=|==|!=|<|>)[^()]*)\)(\s*[*+/\-]))");
        result = std::regex_replace(result, re, "$1float($2)$3");
    }

    // Fix: "float_var *= bool_var" → "float_var *= float(bool_var)"
    // HLSL allows bool in arithmetic (true=1.0, false=0.0); GLSL does not.
    // Collect all local bool variable names, then wrap them with float() when used
    // in compound assignment (*=, +=, -=, /=) or after arithmetic operators (* + - /).
    {
        std::set<std::string> bool_vars;
        static const std::regex            re_bool(R"(\bbool\s+(\w+)\s*[=;])");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_bool);
             it != std::sregex_iterator();
             ++it)
            bool_vars.insert((*it)[1].str());
        for (const auto& bv : bool_vars) {
            // Wrap bool var after *= += -= /= operators
            std::regex re_compound(R"(([*+\-/]=\s*)\b)" + bv + R"(\b(?!\s*[.(]))");
            result = std::regex_replace(result, re_compound, "$1float(" + bv + ")");
            // Wrap bool var after binary * + - / operators (preceded by space or paren)
            std::regex re_arith(R"(([*+\-/]\s*)\b)" + bv + R"(\b(?!\s*[.(=]))");
            result = std::regex_replace(result, re_arith, "$1float(" + bv + ")");
        }
    }

    // Fix: "int VAR = step(EXPR)" → "float VAR = step(EXPR)"
    // step() returns genType (float); the variable is used in float arithmetic throughout
    // (bar *= step(...), bar * u_BarOpacity, etc.), so changing the type is correct.
    // HLSL allows int = float implicitly; GLSL requires matching types.
    {
        static const std::regex re(R"(\bint\s+(\w+)\s*=\s*(step\s*\([^;]*\))\s*;)");
        result = std::regex_replace(result, re, "float $1 = $2;");
    }

    // Fix: HLSL scalar swizzle `float_var.xy / .xyz / .xyzw` for broadcast.
    // Driver: workshop effect 2487531853 "Lens Flare Sun" (5 wallpapers —
    // Naruto family) uses `timer.xy * timer2.xyz` where timer and timer2
    // are floats; HLSL silently broadcasts `s.xy` to `vec2(s, s)`.  GLSL
    // rejects scalar swizzles.
    //
    // Collect known-float local variable names, then rewrite `name.xy`
    // (or `.xyz`/`.xyzw`/`.rg`/`.rgb`/`.rgba`) to `vecN(name)`.
    //
    // NOTE: when a varying or uniform `vec4 NAME` is shadowed by a local
    // `float NAME` (Lens Flare Sun: `varying vec4 timer; ... float timer = ...`)
    // we still rewrite all `NAME.xy/.xyz/.xyzw` since GLSL rejects swizzling
    // even on the vec4 reference if the local re-declaration is in scope —
    // glslang takes the FLOAT and rejects.  The wallpaper author's HLSL
    // pattern is "use the local float, broadcast via `.xy`" so the rewrite
    // is what they intended.
    {
        std::set<std::string> float_locals;
        static const std::regex            re_decl(R"(\bfloat\s+(\w+)\s*[=;])");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_decl);
             it != std::sregex_iterator(); ++it)
            float_locals.insert((*it)[1].str());
        // Also collect uniforms/varyings declared `float NAME`
        static const std::regex re_qual(R"(\b(?:uniform|in|out|varying|attribute)\s+float\s+(\w+)\s*;)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_qual);
             it != std::sregex_iterator(); ++it)
            float_locals.insert((*it)[1].str());
        // Per-name swizzle expansion
        for (const auto& name : float_locals) {
            // `name.xy` / `.rg` → vec2(name)
            std::regex r2(R"(\b)" + name + R"(\.(?:xy|rg)\b)");
            result = std::regex_replace(result, r2, "vec2(" + name + ")");
            // `name.xyz` / `.rgb` → vec3(name)
            std::regex r3(R"(\b)" + name + R"(\.(?:xyz|rgb)\b)");
            result = std::regex_replace(result, r3, "vec3(" + name + ")");
            // `name.xyzw` / `.rgba` → vec4(name)
            std::regex r4(R"(\b)" + name + R"(\.(?:xyzw|rgba)\b)");
            result = std::regex_replace(result, r4, "vec4(" + name + ")");
        }
    }

    // Fix: HLSL implicit vector→scalar truncation in `float VAR = vecN_expr;`.
    // Driver: workshop effect 2487531853 "Lens Flare Sun" (5 wallpapers —
    // Naruto family) ships
    //   `float pointer = g_PointerPosition.xy * u_pointerSpeed;`
    // — the RHS is vec2 but the author wrote `float`.  HLSL silently
    // truncates to .x; glslang rejects.
    //
    // Heuristic: if the RHS contains a multi-component swizzle access
    // (`.xy`, `.xyz`, `.xyzw`, or `.rg/.rgb/.rgba`) and does NOT already end
    // with a single-component swizzle (`.x`, `.y`, …), wrap the RHS in
    // parentheses and append `.x`.  Skip pure assignments that already
    // produce a scalar (e.g. `float a = b.x;`) so we don't double-wrap.
    //
    // Collect user-defined functions whose return type is `float` so we
    // can skip wrapping `float NAME = userFloatFn(args.with.swizzles);` —
    // Voyager Light Pillar (3697335780, workshop 3647838840) has
    // `float noise(vec2 coord) {...}` and uses `float rnd = noise(gl_Position.xy);`.
    std::set<std::string> user_float_fns;
    {
        static const std::regex re_fn(R"(\bfloat\s+(\w+)\s*\([^)]*\)\s*\{)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_fn);
             it != std::sregex_iterator(); ++it)
            user_float_fns.insert((*it)[1].str());
    }
    {
        static const std::regex re(R"(\bfloat\s+(\w+)\s*=\s*([^;]+);)");
        std::string out;
        std::string::size_type lastPos = 0;
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
             it != std::sregex_iterator(); ++it) {
            const auto& m = *it;
            const std::string rhs = m[2].str();
            // Skip if RHS is a simple scalar pattern (no multi-component
            // swizzle).  Look for `.xy`/`.xyz`/`.xyzw`/`.rg`/`.rgb`/`.rgba`
            // — note `.x`/`.y`/etc. alone are scalar and ignored by this
            // regex (negative-lookahead-like via length match).
            static const std::regex multi_sw(R"(\.(xy|xyz|xyzw|rg|rgb|rgba)\b)");
            if (! std::regex_search(rhs, multi_sw)) continue;
            // Skip if RHS already ends with .x/.y/.z/.w/.r/.g/.b/.a
            // (already-truncated forms like `(...).x`).
            static const std::regex trailing_scalar(R"(\.[xyzwrgba]\s*$)");
            if (std::regex_search(rhs, trailing_scalar)) continue;
            // Skip texture() calls (already handled by an earlier rule).
            if (rhs.find("texture") != std::string::npos &&
                rhs.find("texture") < 6) continue;
            // Skip vec*() / float() constructor (already explicit cast).
            static const std::regex ctor_only(R"(^\s*(vec[234]|float)\s*\()");
            if (std::regex_search(rhs, ctor_only)) continue;
            // Skip when the RHS starts with a known scalar-returning
            // function call (length/dot/distance/_wedot/_wep, etc.) —
            // appending `.x` to a scalar is a glslang error.  Cyberpunk
            // Lucy 2866203962 color_key effect: `float delta = _wedot(...);`
            // — the multi-component swizzle is inside _wedot's args, not
            // the outer expression.
            static const std::regex scalar_call(
                R"(^\s*(length|dot|distance|_wedot|_wep|_wemx|_wemn|abs|min|max|clamp|saturate|smoothstep|step|sign|floor|ceil|fract|mod|sin|cos|tan|asin|acos|atan|exp|log|log10|sqrt|inversesqrt|trunc|round)\s*\()");
            if (std::regex_search(rhs, scalar_call)) continue;
            // Skip when the RHS starts with a user-defined float-returning
            // function (collected above).
            {
                bool is_user_float_call = false;
                for (const auto& fn : user_float_fns) {
                    std::regex re(R"(^\s*)" + fn + R"(\s*\()");
                    if (std::regex_search(rhs, re)) {
                        is_user_float_call = true;
                        break;
                    }
                }
                if (is_user_float_call) continue;
            }

            out.append(result, lastPos, m.position() - lastPos);
            out.append("float ").append(m[1].str()).append(" = (")
                .append(rhs).append(").x;");
            lastPos = m.position() + m.length();
        }
        if (lastPos > 0) {
            out.append(result, lastPos, std::string::npos);
            result = std::move(out);
        }
    }

    // Fix: texture(sampler2D, VEC4_VAR) → texture(sampler2D, VEC4_VAR.xy)
    // HLSL varyings may be declared as vec4 (float4 semantics) and used bare as texture
    // coordinates. sampler2D texture() requires vec2.  Add .xy for any vec4/vec3 varying
    // used bare (no existing swizzle) as a texture() coordinate.
    {
        // Collect all "in vec4" and "in vec3" varying names
        std::set<std::string> wide_varyings;
        static const std::regex            re_in(R"(\bin\s+vec[34]\s+(\w+)\s*;)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_in);
             it != std::sregex_iterator();
             ++it)
            wide_varyings.insert((*it)[1].str());
        for (const auto& name : wide_varyings) {
            // Match texture(SAMPLER, NAME) where NAME is NOT followed by '.'
            std::regex re_tex(R"(\btexture\s*\(\s*(\w+)\s*,\s*)" + name + R"(\s*\))");
            result = std::regex_replace(result, re_tex, "texture($1, " + name + ".xy)");
        }
    }

    // Fix: "const TYPE VAR = texture(...)" → remove const qualifier.
    // GLSL requires const initializers to be compile-time constants; texture() is runtime.
    {
        static const std::regex re(R"(\bconst\s+(vec[234]|float|int)\s+(\w+)\s*=\s*(texture\w*\s*\())");
        result = std::regex_replace(result, re, "$1 $2 = $3");
    }

    // Fix: "const vecN VAR = …<uniform_name>…;" — drop const when the
    // initializer references any `uniform` identifier.  GLSL only allows
    // compile-time-constant initializers for global const; HLSL was lax.
    // Driver: Jett × Jinx (3031735486) ships
    //   const vec2 type = vec2(g_Texture0Resolution.x/g_Texture0Resolution.y, 1.0)
    //                     * vec2(g_ratio, 1.0);
    // — both factors involve uniforms.  Pre-fix glslang reported
    //   "global const initializers must be constant 2-component vector of float"
    // and the renderer SEGV'd downstream.
    {
        std::set<std::string> uniform_names;
        static const std::regex            re_uni(R"(\buniform\s+(?:\w+\s+)?(\w+)\s*(?:\[[^\]]*\])?\s*;)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_uni);
             it != std::sregex_iterator(); ++it)
            uniform_names.insert((*it)[1].str());

        // Walk const-vec[234] decls and drop `const` if RHS mentions any uniform.
        static const std::regex re_const_vec(R"(\bconst\s+(vec[234])\s+(\w+)\s*=\s*([^;]+);)");
        std::string out;
        size_t lastPos = 0;
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_const_vec);
             it != std::sregex_iterator(); ++it) {
            const auto& m   = *it;
            std::string rhs = m[3].str();
            bool        uses_uniform = false;
            for (const auto& un : uniform_names) {
                std::regex re_use(R"(\b)" + un + R"(\b)");
                if (std::regex_search(rhs, re_use)) { uses_uniform = true; break; }
            }
            if (! uses_uniform) continue;
            out.append(result, lastPos, m.position() - lastPos);
            out += m[1].str() + " " + m[2].str() + " = " + rhs + ";";
            lastPos = m.position() + m.length();
        }
        if (lastPos > 0) {
            out.append(result, lastPos, std::string::npos);
            result = std::move(out);
        }
    }

    // Fix: writing to 'in' varying — HLSL allows mutable inputs; GLSL doesn't.
    // Create a mutable copy for any 'in' variable that is assigned to (plain or compound).
    // Skip varyings that are *shadowed* by a local declaration (e.g.
    //   `vec4 rValue = texture(...);` inside main) — those aren't mutating the
    // varying, they're introducing a local with the same name.  Renaming would
    // cause a redefinition clash with the mutable-copy we'd insert at the top.
    {
        static const std::regex re_in(R"(\bin\s+(vec[234]|float)\s+(\w+)\s*;)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_in);
             it != std::sregex_iterator();
             ++it) {
            std::string type = (*it)[1].str();
            std::string name = (*it)[2].str();

            // Shadow check: a LOCAL declaration `TYPE name [= …];` anywhere.
            // Must exclude the varying's own `in TYPE name;` decl, and
            // uniform/out/attribute/varying qualifiers.  C++ regex has no
            // variable-length lookbehind, so we inspect preceding context
            // per match.
            bool       shadowed = false;
            std::regex re_decl(R"(\b(?:vec[234]|float|int|uint|bool|mat[234](?:x[234])?)\s+)" +
                               name + R"(\s*(?:=|;|,|\)))");
            for (auto dit = std::sregex_iterator(result.begin(), result.end(), re_decl);
                 dit != std::sregex_iterator();
                 ++dit) {
                auto pos = (size_t)dit->position();
                // Look back ~20 chars for a qualifier keyword.
                size_t                  start = pos > 20 ? pos - 20 : 0;
                std::string             ctx   = result.substr(start, pos - start);
                static const std::regex re_qual(
                    R"(\b(?:in|uniform|out|attribute|varying|flat\s+in|centroid\s+in|smooth\s+in|noperspective\s+in)\s+$)");
                if (! std::regex_search(ctx, re_qual)) {
                    shadowed = true;
                    break;
                }
            }
            if (shadowed) continue;

            // Detect plain assignment (NAME = ...), component (NAME.x = ...) or
            // compound (NAME +=/-=/*=//=), but not == (comparison).
            std::regex re_assign("\\b" + name + R"((\.\w+)?\s*[\+\-\*\/]?=(?!=))");
            if (! std::regex_search(result, re_assign)) continue;

            // Rename all body uses: NAME → _m_NAME, then fix the 'in' declaration back
            std::string mut = "_m_" + name;
            result          = std::regex_replace(result, std::regex("\\b" + name + "\\b"), mut);
            // Restore the 'in' declaration
            result = std::regex_replace(result,
                                        std::regex("\\bin\\s+" + type + "\\s+" + mut + "\\s*;"),
                                        "in " + type + " " + name + ";");
            // Add mutable copy at start of main()
            result = std::regex_replace(result,
                                        std::regex(R"(void\s+main\s*\(\s*\)\s*\{)"),
                                        "void main() {\n " + type + " " + mut + " = " + name + ";");
            break; // handle one at a time to avoid iterator invalidation
        }
    }

    // Fix: "vec3 VAR = vec4(EXPR)" or "vec3 VAR = texture(...)" → append .xyz
    // HLSL implicit truncation from vec4 result to vec3.  Handles both the vec4()
    // constructor and the texture() family (all return vec4).
    {
        static const std::regex re(R"(\bvec3\s+(\w+)\s*=\s*((?:vec4|texture\w*)\s*\([^;]*?\))\s*;)");
        result = std::regex_replace(result, re, "vec3 $1 = $2.xyz;");
    }

    // Fix: vec2_named OP <expr_with_3_swizzle> — HLSL broadcasts vec2 to vec3
    // by appending 0.  GLSL rejects vec2+vec3 directly.  Driver: workshop
    // effect 2892816289 dot_matrix_mobile_fix.frag (used by Daisies
    // 3501635854, Bush N' Hydrangea 3411135006, Bush N' Roses 3335022037,
    // Chill Time 2925278995, Into the Starfield 3028310305) ships
    //   vec2 p = …, e = vec2(thickness, 0.0);
    //   tetraNoise(p + e.xyy);  // e.xyy is vec3 → vec2+vec3 mismatch
    // We rewrite `p + e.xyy` to `vec3(p, 0.0) + e.xyy`.  Same for `-`/`*`/`/`.
    {
        std::set<std::string> vec2_names;
        static const std::regex re_v2(R"(\bvec2\s+(\w+)\s*[=;,)])");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_v2);
             it != std::sregex_iterator(); ++it)
            vec2_names.insert((*it)[1].str());
        // Drop function/keyword names so we don't accidentally treat them as
        // variable references (`vec2 max(...)` would put `max` in this set).
        // Function declarations: `vec2 NAME(...)` — exclude names followed by `(`.
        {
            std::set<std::string> drop;
            for (const auto& n : vec2_names) {
                std::regex re_fn(R"(\bvec2\s+)" + n + R"(\s*\()");
                if (std::regex_search(result, re_fn)) drop.insert(n);
            }
            for (const auto& d : drop) vec2_names.erase(d);
        }

        // Scope-aware: for each occurrence of `vname OP 3-swizzle`, walk
        // backward to find the nearest preceding `vec[234] vname` decl.
        // Only rewrite when that nearest decl is vec2.  Daisies (3501635854)
        // shows the canonical shadowed-name case: `vec3 p` in hsv2rgb and
        // `vec2 p` in main(); we want to wrap p in main but leave it alone
        // in hsv2rgb.
        auto nearest_decl_width = [&](size_t match_pos, const std::string& vname) -> int {
            std::regex re_any(R"(\b(vec[234])\s+)" + vname + R"(\b)");
            int        best_w = 0;
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_any);
                 it != std::sregex_iterator(); ++it) {
                size_t p = (size_t)it->position();
                if (p >= match_pos) break;
                std::string typ = (*it)[1].str();
                if (typ == "vec2") best_w = 2;
                else if (typ == "vec3") best_w = 3;
                else if (typ == "vec4") best_w = 4;
            }
            return best_w;
        };

        // Function signature awareness: if the match site is the FIRST arg of
        // a user function that takes a vec2 (e.g. Daisies' tetraNoise),
        // truncate the 3-swizzle to 2 instead of broadcasting.  Walk forward
        // from `vname` looking for a function name preceding a `(`.
        std::map<std::string, int> user_fn_arg1_width;
        {
            static const std::regex re_fn(
                R"(\b(?:float|int|uint|bool|void|vec[234])\s+(\w+)\s*\(\s*(?:in\s+)?(vec[234])\s+\w+)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_fn);
                 it != std::sregex_iterator(); ++it) {
                std::string typ  = (*it)[2].str();
                int         w    = (typ == "vec2") ? 2 : (typ == "vec3" ? 3 : 4);
                user_fn_arg1_width[(*it)[1].str()] = w;
            }
        }
        // Returns the function name whose call we're inside at `pos`, if any.
        auto enclosing_call = [&](size_t pos) -> std::string {
            // Walk backwards: track depth, find `name(` whose depth at pos is 1.
            // For simplicity, find the nearest preceding `(` that's not yet
            // closed at pos, then read the identifier before it.
            int    depth = 0;
            ssize_t open = -1;
            for (ssize_t i = (ssize_t)pos - 1; i >= 0; --i) {
                char c = result[i];
                if (c == ')') ++depth;
                else if (c == '(') {
                    if (depth == 0) { open = i; break; }
                    --depth;
                }
            }
            if (open < 0) return {};
            ssize_t e = open;
            while (e > 0 && std::isspace((unsigned char)result[e - 1])) --e;
            ssize_t s = e;
            while (s > 0 && (std::isalnum((unsigned char)result[s - 1]) ||
                             result[s - 1] == '_'))
                --s;
            if (s == e) return {};
            return result.substr((size_t)s, (size_t)(e - s));
        };

        for (const auto& vname : vec2_names) {
            // LHS form
            std::regex re_lhs(R"(\b)" + vname +
                              R"(\b(\s*[+\-*/]\s*)(\w+)\.([xyzwrgba]{3})\b)");
            std::string out;
            out.reserve(result.size());
            size_t lastPos = 0;
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_lhs);
                 it != std::sregex_iterator(); ++it) {
                size_t pos = (size_t)it->position();
                if (nearest_decl_width(pos, vname) != 2) continue;
                std::string op       = (*it)[1].str();
                std::string other    = (*it)[2].str();
                std::string swiz     = (*it)[3].str();
                std::string fn       = enclosing_call(pos);
                auto        fit      = user_fn_arg1_width.find(fn);
                bool        callee2  = (fit != user_fn_arg1_width.end() && fit->second == 2);
                out.append(result, lastPos, pos - lastPos);
                if (callee2) {
                    // Function expects vec2 — truncate swizzle to 2.
                    out.append(vname).append(op).append(other)
                       .append(".").append(swiz.substr(0, 2));
                } else {
                    // Broadcast vec2 to vec3 to match the swizzle.
                    out.append("vec3(").append(vname).append(", 0.0)")
                       .append(op).append(other).append(".").append(swiz);
                }
                lastPos = pos + (size_t)it->length();
            }
            out.append(result, lastPos, std::string::npos);
            result = std::move(out);
            // RHS form
            std::regex re_rhs(R"((\w+)\.([xyzwrgba]{3})(\s*[+\-*/]\s*)\b)" + vname +
                              R"(\b)");
            out.clear();
            out.reserve(result.size());
            lastPos = 0;
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_rhs);
                 it != std::sregex_iterator(); ++it) {
                size_t pos = (size_t)it->position();
                size_t vpos = pos + (size_t)((*it)[1].length() + 1 + (*it)[2].length() +
                                              (*it)[3].length());
                if (nearest_decl_width(vpos, vname) != 2) continue;
                std::string other    = (*it)[1].str();
                std::string swiz     = (*it)[2].str();
                std::string op       = (*it)[3].str();
                std::string fn       = enclosing_call(pos);
                auto        fit      = user_fn_arg1_width.find(fn);
                bool        callee2  = (fit != user_fn_arg1_width.end() && fit->second == 2);
                out.append(result, lastPos, pos - lastPos);
                if (callee2) {
                    out.append(other).append(".").append(swiz.substr(0, 2))
                       .append(op).append(vname);
                } else {
                    out.append(other).append(".").append(swiz).append(op)
                       .append("vec3(").append(vname).append(", 0.0)");
                }
                lastPos = pos + (size_t)it->length();
            }
            out.append(result, lastPos, std::string::npos);
            result = std::move(out);
        }
    }

    // Fix: "vec<narrow> VAR = wider_var;" — HLSL narrowing assignment.
    // Driver: 2026-05-15 audit found vec4-varying-to-vec2-local patterns in
    // multiple wallpapers:
    //   * NieR 2B-ish chromatic_aberration: `vec2 _m_v_TexCoord = v_TexCoord;`
    //     where `in vec4 v_TexCoord;` lives.
    //   * Generic FBO compose: `vec3 col = some_vec4_uniform;`.
    // Plain `=` doesn't go through the existing arithmetic-truncation pass
    // (which requires an operator).  Detect the LHS rank from the
    // declaration, look up the RHS identifier in the vec3/vec4 uniform/in
    // sets, and inject `.xy` / `.xyz` accordingly.
    {
        auto collectByQual = [&](int width) {
            std::set<std::string> names;
            // Match `uniform vecN NAME;` / `in vecN NAME;` / `varying vecN NAME;`
            // and local `vecN NAME` declarations.
            std::regex re(std::string(R"(\b(?:uniform|in|out|varying)\s+vec)") +
                          std::to_string(width) + R"(\s+(\w+)\s*[;\[])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                 it != std::sregex_iterator(); ++it)
                names.insert((*it)[1].str());
            return names;
        };
        const auto vec3_named = collectByQual(3);
        const auto vec4_named = collectByQual(4);

        auto rewriteAssign = [&](int lhsW, int rhsW, const char* swizzle,
                                 const std::set<std::string>& rhsNames) {
            const std::string lhsT = "vec" + std::to_string(lhsW);
            for (const auto& rname : rhsNames) {
                std::regex re(R"(\b)" + lhsT + R"(\s+(\w+)\s*=\s*)" + rname +
                              R"(\b(?![.\[(\w])\s*;)");
                result = std::regex_replace(result, re,
                                            lhsT + " $1 = " + rname + "." + swizzle + ";");
            }
            (void)rhsW;
        };
        // vec2 X = vec3_var; → .xy ; vec2 X = vec4_var; → .xy
        rewriteAssign(2, 3, "xy",  vec3_named);
        rewriteAssign(2, 4, "xy",  vec4_named);
        // vec3 X = vec4_var; → .xyz
        rewriteAssign(3, 4, "xyz", vec4_named);

        // Belt-and-braces: same pattern via string-find rather than regex.
        // Some shader contents trigger std::regex backtracking pathologies on
        // large inputs and end up not matching the same text that an isolated
        // regex_search does match.  rhythm in garden (3496072356), Fami
        // Chainsaw Man (3034862641), Adventurous Skyscape (3036962127) all
        // ship `vec2 fragPos = v_TexCoord;` (where v_TexCoord is `in vec4`)
        // and regex-based rewriteAssign demonstrably missed it.
        //
        // Search for exact "vec<lhsW> NAME = wider_name;" forms by string,
        // verify the trailing context (`;` with optional whitespace) and the
        // leading context (preceded by `;`, `}`, `{`, `\n`), then splice in
        // the swizzle.
        auto rewriteAssignByString = [&](int lhsW, const char* swizzle,
                                         const std::set<std::string>& rhsNames) {
            const std::string lhsT = "vec" + std::to_string(lhsW);
            for (const auto& rname : rhsNames) {
                const std::string needle_tail = "= " + rname + ";";
                std::string out;
                out.reserve(result.size() + 16);
                size_t pos = 0;
                while (true) {
                    size_t p = result.find(needle_tail, pos);
                    if (p == std::string::npos) {
                        out.append(result, pos, std::string::npos);
                        break;
                    }
                    // Walk backward to find the preceding token: must be an
                    // identifier (VAR), then whitespace, then `vec<lhsW>`,
                    // then whitespace or line-start.
                    size_t q = p;
                    while (q > 0 && std::isspace((unsigned char)result[q - 1])) --q;
                    size_t var_end = q;
                    while (q > 0 &&
                           (std::isalnum((unsigned char)result[q - 1]) || result[q - 1] == '_'))
                        --q;
                    if (q == var_end) { // no identifier — copy literally
                        out.append(result, pos, p - pos + needle_tail.size());
                        pos = p + needle_tail.size();
                        continue;
                    }
                    size_t var_beg = q;
                    while (q > 0 && std::isspace((unsigned char)result[q - 1])) --q;
                    size_t type_end = q;
                    while (q > 0 &&
                           (std::isalnum((unsigned char)result[q - 1]) || result[q - 1] == '_'))
                        --q;
                    if (type_end - q != lhsT.size() ||
                        result.compare(q, lhsT.size(), lhsT) != 0) {
                        out.append(result, pos, p - pos + needle_tail.size());
                        pos = p + needle_tail.size();
                        continue;
                    }
                    // Leading context: must be punctuation/newline/start, not
                    // identifier characters (otherwise `myvec2 foo = v_TexCoord;`).
                    if (q > 0) {
                        char prev = result[q - 1];
                        if (std::isalnum((unsigned char)prev) || prev == '_') {
                            out.append(result, pos, p - pos + needle_tail.size());
                            pos = p + needle_tail.size();
                            continue;
                        }
                    }
                    // Splice — keep everything up to `;`, then inject swizzle.
                    out.append(result, pos, p - pos);
                    out.append("= ");
                    out.append(rname);
                    out.append(".");
                    out.append(swizzle);
                    out.append(";");
                    pos = p + needle_tail.size();
                }
                result = std::move(out);
            }
        };
        rewriteAssignByString(2, "xy",  vec3_named);
        rewriteAssignByString(2, "xy",  vec4_named);
        rewriteAssignByString(3, "xyz", vec4_named);
    }

    // Fix: HLSL scalar→vector broadcast in `vecN VAR = <scalar>;` declarations.
    // Driver: Girl Error System Arona (3341577331) / Daisies (3501635854) /
    // multiple others ship workshop bokeh / chromatic-aberration shaders with
    //   `vec2 radial = 0.0, tangential = 0.0, center = (1.0 - u_center - 0.5) * u_general;`
    // — three vec2 variables initialised from a scalar `0.0` literal.  HLSL
    // broadcasts `0.0` → `float2(0,0)` implicitly; GLSL rejects (`= ' const
    // float' to ' temp highp 2-component vector of float'`).
    //
    // Strategy: split the comma-separated declarator list and wrap each
    // initialiser whose RHS doesn't already look vector-typed.  Conservative:
    // we only touch initialisers whose RHS is composed entirely of
    // scalar-looking tokens (numeric literals + arithmetic + known-scalar
    // identifiers from earlier `uniform float NAME` / `in float NAME` /
    // `float NAME` declarations).  Anything containing `vec[234](`,
    // `texture(`, `.xy/.xyz/.xyzw`, or `.rg/.rgb/.rgba` is left alone — the
    // RHS already produces a vector.
    {
        // Collect known scalar (float) variables from declarations / uniforms.
        // Re-using the same set the swizzle-broadcast pass collects, but locally
        // so this block can run independently.
        std::set<std::string> scalar_vars;
        {
            static const std::regex re_qual(R"(\b(?:uniform|in|out|varying|attribute)\s+float\s+(\w+)\s*[\[;])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_qual);
                 it != std::sregex_iterator(); ++it)
                scalar_vars.insert((*it)[1].str());
            static const std::regex re_local(R"(\bfloat\s+(\w+)\s*[=;])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_local);
                 it != std::sregex_iterator(); ++it)
                scalar_vars.insert((*it)[1].str());
        }
        // Strip a single layer of matching outer parens.  Workshop authors
        // routinely wrap the RHS in redundant `(...)` (e.g.
        // `vec2 noise = (fract(sin(_wedot(...)) * 43758.5453));`).  The outer
        // pair encloses one expression and analytically equivalent to the
        // inner — so peel it before classifying the outer scope.
        auto strip_outer_parens = [](std::string s) {
            for (;;) {
                size_t a = 0, b = s.size();
                while (a < b && std::isspace((unsigned char)s[a])) ++a;
                while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
                if (b - a < 2 || s[a] != '(' || s[b - 1] != ')') break;
                // Check that the outer parens are actually matching (the first
                // '(' matches the last ')' at depth 0).
                int depth = 0;
                bool ok = true;
                for (size_t i = a; i < b; ++i) {
                    if (s[i] == '(') ++depth;
                    else if (s[i] == ')') {
                        --depth;
                        if (depth == 0 && i + 1 < b) { ok = false; break; }
                    }
                }
                if (! ok) break;
                s = s.substr(a + 1, b - a - 2);
            }
            return s;
        };
        // Helper: scan the expression at the OUTER scope (depth 0 relative to
        // expr boundaries — entries into `(...)` increase depth, exits decrease).
        // Returns whether `expr` contains a top-level invocation of any of the
        // always-scalar-returning functions, and (separately) whether any
        // depth-0 vec[234] constructor / texture call appears at the outer
        // scope.  Nested calls (vec2 inside _wedot's args) don't count as
        // vector producers because they're consumed by the scalar wrapper.
        auto scan_depth0 = [&](const std::string& expr) {
            struct Info { bool has_scalar_fn = false; bool has_vec_producer = false; };
            Info info {};
            int  depth = 0;
            static const std::set<std::string> always_scalar = {
                "_wedot", "dot", "length", "distance",
            };
            for (size_t i = 0; i < expr.size(); ++i) {
                char c = expr[i];
                if (c == '(') {
                    // If preceding token is an identifier, classify this call.
                    if (depth == 0) {
                        size_t j = i;
                        while (j > 0 && std::isspace((unsigned char)expr[j - 1])) --j;
                        size_t end = j;
                        while (j > 0 &&
                               (std::isalnum((unsigned char)expr[j - 1]) || expr[j - 1] == '_'))
                            --j;
                        if (end > j) {
                            std::string fn(expr, j, end - j);
                            if (always_scalar.count(fn)) info.has_scalar_fn = true;
                            if ((fn == "vec2" || fn == "vec3" || fn == "vec4") ||
                                fn.compare(0, 7, "texture") == 0)
                                info.has_vec_producer = true;
                        }
                    }
                    ++depth;
                } else if (c == ')') {
                    --depth;
                }
            }
            return info;
        };
        // Returns true when `expr` (after stripping outer parens) is shaped
        // like `<scalar-yielding-call>(...)` — either directly (e.g. _wedot)
        // or via a chain of rank-preserving wrappers (fract/sin/abs/etc.).
        // Falls back to false for ambiguous shapes; callers go to the broader
        // identifier-scan fallback below.
        std::function<bool(const std::string&)> outermost_scalar_call;
        outermost_scalar_call = [&](const std::string& e) -> bool {
            std::string s = strip_outer_parens(e);
            // Trim
            size_t a = 0, b = s.size();
            while (a < b && std::isspace((unsigned char)s[a])) ++a;
            while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
            if (b - a < 3) return false;
            // Find the first identifier at depth 0 followed by `(`.
            static const std::set<std::string> always_scalar = {
                "_wedot", "dot", "length", "distance",
            };
            static const std::set<std::string> rank_preserving = {
                "fract", "sin", "cos", "tan", "asin", "acos", "atan", "abs",
                "sqrt", "exp", "log", "log2", "floor", "ceil", "round",
                "sign", "negate",
            };
            // First word at start of stripped expression
            size_t p = a;
            while (p < b && (std::isalnum((unsigned char)s[p]) || s[p] == '_')) ++p;
            if (p == a || p == b || s[p] != '(') return false;
            std::string fn(s, a, p - a);
            if (always_scalar.count(fn)) return true;
            if (! rank_preserving.count(fn)) return false;
            // Find matching close-paren for the call.
            int depth = 1;
            size_t arg_start = p + 1, q = p + 1;
            for (; q < b; ++q) {
                if (s[q] == '(') ++depth;
                else if (s[q] == ')') {
                    --depth;
                    if (depth == 0) break;
                }
            }
            if (depth != 0 || q >= b) return false;
            std::string inner(s, arg_start, q - arg_start);
            // Recurse on the arg's "scalar shape".  Arg may include `,` —
            // for our purposes, the first comma-separated piece is the input
            // whose rank propagates.
            int depth2 = 0;
            size_t comma = std::string::npos;
            for (size_t k = 0; k < inner.size(); ++k) {
                if (inner[k] == '(') ++depth2;
                else if (inner[k] == ')') --depth2;
                else if (inner[k] == ',' && depth2 == 0) { comma = k; break; }
            }
            std::string first_arg = (comma == std::string::npos) ? inner : inner.substr(0, comma);
            return outermost_scalar_call(first_arg);
        };

        auto is_scalar_expr = [&](const std::string& expr) -> bool {
            std::string stripped = strip_outer_parens(expr);
            auto info = scan_depth0(stripped);
            // If a definitively-scalar function dominates the outer scope and
            // no other depth-0 vector producer is present, treat the result as
            // scalar even if nested args contain vec[234] constructors.
            if (info.has_scalar_fn && ! info.has_vec_producer) return true;
            // Also check rank-preserving chains via recursion (e.g.
            // `fract(sin(_wedot(...)))` → scalar).
            if (outermost_scalar_call(expr)) return true;
            if (info.has_vec_producer) return false;

            // Multi-component swizzles indicate vector access (foo.xy, foo.rgba, etc.)
            static const std::regex multi_sw(R"(\.(?:xy|xyz|xyzw|rg|rgb|rgba)\b)");
            if (std::regex_search(expr, multi_sw)) return false;
            // Identifiers in the expression — if any unknown identifier appears,
            // we don't know its type, so be conservative and skip.
            static const std::regex id_re(R"([A-Za-z_]\w*)");
            for (auto it = std::sregex_iterator(expr.begin(), expr.end(), id_re);
                 it != std::sregex_iterator(); ++it) {
                std::string id = (*it).str();
                // Recognised scalar built-ins / keywords are fine.
                static const std::set<std::string> scalar_kw = {
                    "true", "false", "abs", "min", "max", "_wemn", "_wemx", "_wep",
                    "sqrt", "pow", "exp", "log", "log2", "sin", "cos", "tan",
                    "asin", "acos", "atan", "floor", "ceil", "round", "fract",
                    "sign", "mod", "step", "smoothstep", "clamp", "mix", "length",
                    "distance", "dot", "_wedot", "saturate", "u_general",
                };
                if (scalar_kw.count(id)) continue;
                if (scalar_vars.count(id)) continue;
                return false;
            }
            return true;
        };
        // Match a declaration starting `vecN NAME = ...;` where vecN is vec2/3/4.
        // The `\w+\s*=` between type and the rest excludes function declarations
        // like `vec3 fnName(args) { ...; }` whose first token after the return
        // type is `(` not `=` — without this guard we wrapped innocent `float
        // AvgLumR = 0.5;` inside the function body in `vec3(0.5)`, because the
        // regex spanned from the return-type vec3 all the way to the first `;`
        // inside the function body.
        static const std::regex re_decl(R"(\b(vec[234])\s+(\w+\s*=[^;]+);)");
        std::string                out;
        std::string::size_type     lastPos = 0;
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_decl);
             it != std::sregex_iterator(); ++it) {
            const auto& m       = *it;
            const std::string vecT = m[1].str();
            const std::string body = m[2].str();
            // Skip when the line is the function-parameter-list opening or a
            // typedef-like return-type (no `=` in body) — this regex only
            // catches statements that contain an initialiser anyway.
            if (body.find('=') == std::string::npos) continue;

            // Walk comma-separated declarators; rebuild only those whose RHS
            // is scalar-only, wrapping with `vecN(...)`.
            std::string              rebuilt;
            std::string::size_type   pos = 0;
            bool                     changed = false;
            int                      depth   = 0;
            std::string::size_type   start   = 0;
            std::vector<std::string> parts;
            for (std::string::size_type i = 0; i < body.size(); ++i) {
                char c = body[i];
                if (c == '(' || c == '[' || c == '{') ++depth;
                else if (c == ')' || c == ']' || c == '}') --depth;
                else if (c == ',' && depth == 0) {
                    parts.push_back(body.substr(start, i - start));
                    start = i + 1;
                }
                (void)pos;
            }
            parts.push_back(body.substr(start));

            for (auto& part : parts) {
                // Each `part` is `NAME = EXPR` (or just `NAME` — no init).
                auto eq = part.find('=');
                if (eq == std::string::npos) continue;
                std::string lhs = part.substr(0, eq);
                std::string rhs = part.substr(eq + 1);
                // Trim ASCII whitespace
                auto trim = [](std::string& s) {
                    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
                    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
                };
                std::string rhs_t = rhs;
                trim(rhs_t);
                if (rhs_t.empty()) continue;
                if (! is_scalar_expr(rhs_t)) continue;
                // Wrap
                std::string wrapped = " " + vecT + "(" + rhs_t + ")";
                part = lhs + "=" + wrapped;
                changed = true;
            }
            if (! changed) continue;
            // Reassemble
            std::string new_body;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) new_body += ",";
                new_body += parts[i];
            }
            std::string new_decl = vecT + " " + new_body + ";";
            out.append(result, lastPos, (std::string::size_type)m.position() - lastPos);
            out += new_decl;
            lastPos = (std::string::size_type)m.position() + (std::string::size_type)m.length();
        }
        out.append(result, lastPos, std::string::npos);
        result = std::move(out);
    }

    // Fix: `vecN VAR = … wider_var …;` (arithmetic context) — inject swizzle on
    // wider_var to match the LHS rank.  HLSL truncates implicitly; GLSL errors.
    //
    // Driver: Arona (3341577331) chromatic_aberration vertex shader:
    //   out vec4 v_PointerUV;
    //   vec2 da = v_PointerUV * (g_Scale * g_Scale_FollowCursor_Multiplier) * 0.001;
    // The existing wider-varying transform (line ~1500) only handles bare
    // adjacency (`wname OP nn` or `wname OP vecN(...)`) — parenthesized
    // expressions like `(g_Scale * …)` aren't matched.  But once we know the
    // declaration is `vec2 da = …`, anything inside RHS at the same scope
    // must be vec2.  Walking the RHS, we can blindly swizzle known wider
    // varyings to LHS rank because HLSL's implicit truncation would do the
    // same thing.
    //
    // Restrictions:
    //   * Only triggers when wider_var appears bare (no `.x`/`.xy`/`[`/`(`).
    //   * Skips when wider_var is the SUB-EXPRESSION of a function arg whose
    //     parameter is known wider (no introspection — so this is an
    //     uncertain case).  Mitigated by only inspecting straight assignment
    //     RHS, never inside function-call args.
    {
        std::map<std::string, int> wider_named;  // name → rank (3 or 4)
        static const std::regex re_wider(R"(\b(?:uniform|in|out|varying)\s+vec([34])\s+(\w+)\s*[;\[])");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_wider);
             it != std::sregex_iterator(); ++it)
            wider_named[(*it)[2].str()] = std::stoi((*it)[1].str());
        // Also locals: `vec3 NAME = …;` and `vec4 NAME = …;`.
        static const std::regex re_local_wider(R"(\bvec([34])\s+(\w+)\s*[=;])");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_local_wider);
             it != std::sregex_iterator(); ++it)
            wider_named[(*it)[2].str()] = std::stoi((*it)[1].str());

        // Drop varyings that are SHADOWED by a local float — bare `name`
        // resolves to the float, so swizzling produces scalar-swizzle errors.
        // Mirrors the existing wider-varying transform.  Driver: Lens Flare
        // Sun (`varying vec4 timer; float timer = sin(...);`).
        {
            std::set<std::string> shadowed;
            static const std::regex re_float(R"(\bfloat\s+(\w+)\s*[=;])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_float);
                 it != std::sregex_iterator(); ++it)
                shadowed.insert((*it)[1].str());
            for (const auto& s : shadowed) wider_named.erase(s);
        }

        // For each `vec[23] VAR = RHS;` declaration, scan RHS for wider names.
        static const std::regex re_narrow_decl(R"(\bvec([23])\s+\w+\s*=\s*([^;]+);)");
        std::string out;
        out.reserve(result.size());
        size_t lastPos = 0;
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_narrow_decl);
             it != std::sregex_iterator(); ++it) {
            const auto& m       = *it;
            int         lhs_n   = std::stoi(m[1].str());
            std::string rhs     = m[2].str();
            std::string new_rhs = rhs;
            bool        any     = false;
            for (const auto& [wname, wrank] : wider_named) {
                if (wrank <= lhs_n) continue;
                std::string sw = (lhs_n == 2) ? "xy" : "xyz";
                // Only inject swizzle when `wname` is adjacent to an
                // arithmetic operator (i.e. participates in arithmetic).
                // Skip bare uses (e.g. passed as a function arg) — the
                // callee may need the wider value, and the existing test
                // suite contract guards "vec4 varying NOT truncated when
                // passed bare to function".
                //
                // Pattern: `[*+/-]\s*wname\b` (RHS of an op) or
                //          `\bwname\s*[*+/-]` (LHS of an op).
                std::regex re_use_rhs(R"(([+\-*/]\s*))" + wname +
                                      R"(\b(?![.\[(\w]))");
                std::string after = std::regex_replace(new_rhs, re_use_rhs,
                                                       "$1" + wname + "." + sw);
                std::regex re_use_lhs(R"(\b)" + wname +
                                      R"(\b(?![.\[(\w])(\s*[+\-*/]))");
                after = std::regex_replace(after, re_use_lhs,
                                           wname + "." + sw + "$1");
                if (after != new_rhs) {
                    new_rhs = std::move(after);
                    any     = true;
                }
            }
            if (! any) continue;
            // Find LHS prefix (before the captured RHS).  We reconstruct the
            // declaration by inserting the new RHS in place of the old one.
            std::string lhs_text(result, m.position(), m.position(1) + m.length(1) +
                                                            (m.position(2) - m.position(1) -
                                                             m.length(1)));
            // Simpler: replace the whole match using the captured pieces.
            out.append(result, lastPos,
                       (std::string::size_type)m.position() - lastPos);
            // Recompose: vec<lhs_n> NAME = NEW_RHS;
            std::string vec_t = "vec" + std::to_string(lhs_n);
            // Recover the original NAME by stripping vec[23] + ws + then up to '='.
            std::string match_str = m.str();
            auto eq = match_str.find('=');
            std::string name_part(match_str, 0, eq);
            // name_part still starts with "vecN " — keep as-is, just swap RHS.
            out += name_part + "= " + new_rhs + ";";
            lastPos = (std::string::size_type)m.position() + m.length();
        }
        out.append(result, lastPos, std::string::npos);
        result = std::move(out);
    }

    // Fix: "float VAR = VEC_EXPR" → "float VAR = (VEC_EXPR).x"
    // HLSL implicitly takes the first component; GLSL requires explicit swizzle.
    // Detect known vec2/vec3/vec4 variables used in a float assignment.
    {
        std::set<std::string> vec_vars;
        static const std::regex            re_vec(R"(\b(?:uniform|in)\s+vec[234]\s+(\w+)\s*;)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_vec);
             it != std::sregex_iterator();
             ++it)
            vec_vars.insert((*it)[1].str());
        for (const auto& name : vec_vars) {
            // float VAR = VEC_NAME * EXPR;  →  float VAR = VEC_NAME.x * EXPR;
            std::regex re(R"(\bfloat\s+(\w+)\s*=\s*)" + name + R"(\s*([*+\-/]))");
            result = std::regex_replace(result, re, "float $1 = " + name + ".x $2");
            // float VAR = EXPR * VEC_NAME;  →  float VAR = EXPR * VEC_NAME.x;
            // Must scope to float-assignment context — otherwise this wrongly appends .x
            // to vec uniforms at the tail of any statement (e.g. "vec2 x = a - u_Offset;"
            // became "vec2 x = a - u_Offset.x;", introducing a type mismatch).
            std::regex re2(R"((\bfloat\s+\w+\s*=\s*[^;]*[*+\-/]\s*))" + name + R"((\s*;))");
            result = std::regex_replace(result, re2, "$1" + name + ".x$2");
        }
    }

    // Fix: wider `in`/`out` varying arithmetically adjacent to a known narrower vec var.
    // HLSL auto-truncates vec4→vec2 silently; GLSL errors on vec4 + vec2.
    // Works everywhere (inside texture() calls, nested expressions, etc.) because
    // the heuristic requires the narrower operand to be named and adjacent.
    //
    // `out` is included for vertex shaders that compute partial-component
    // output then re-use the varying as a temp source.  Driver: Arona
    // (3341577331) chromatic_aberration vertex shader does
    //     v_PointerUV.xyz = …; v_PointerUV.xy *= 0.5; v_PointerUV.xy /= v_PointerUV.z;
    //     vec2 da = v_PointerUV * (g_Scale * g_Scale_FollowCursor_Multiplier) * 0.001;
    // where v_PointerUV is `out vec4`.  HLSL truncates `v_PointerUV` to vec2
    // when multiplying by a vec2; GLSL rejects.
    {
        std::map<std::string, int> wider_varyings;
        {
            static const std::regex re_wide(R"(\b(?:in|out)\s+vec([34])\s+(\w+)\s*;)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_wide);
                 it != std::sregex_iterator();
                 ++it)
                wider_varyings[(*it)[2].str()] = std::stoi((*it)[1].str());
        }
        // Skip varyings whose name is also declared as a local float — the
        // local shadows the varying so adding a swizzle to bare uses
        // produces "scalar swizzle: not supported" (Lens Flare Sun ships
        // `varying vec4 timer; float timer = sin(...);`).  In that case the
        // bare `timer` resolves to the float local, not the varying — no
        // truncation needed.
        {
            std::set<std::string> shadowed;
            static const std::regex re_float(R"(\bfloat\s+(\w+)\s*[=;])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_float);
                 it != std::sregex_iterator(); ++it)
                shadowed.insert((*it)[1].str());
            for (const auto& s : shadowed) wider_varyings.erase(s);
        }
        auto collectByWidth = [&](int width) {
            std::set<std::string> names;
            std::regex re(std::string(R"(\b(?:uniform|in|varying)\s+vec)") + std::to_string(width) +
                          R"(\s+(\w+)\s*;|\bvec)" + std::to_string(width) + R"(\s+(\w+)\s*[;=])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                 it != std::sregex_iterator();
                 ++it) {
                std::string n = (*it)[1].str();
                if (n.empty()) n = (*it)[2].str();
                if (! n.empty()) names.insert(std::move(n));
            }
            return names;
        };
        const auto vec2_names = collectByWidth(2);
        const auto vec3_names = collectByWidth(3);

        auto applyTrunc = [&](const std::string&           wname,
                              const std::set<std::string>& narrowNames,
                              const char*                  swizzle) {
            for (const auto& nn : narrowNames) {
                if (nn == wname) continue;
                // W OP N (bare both sides)
                std::regex re_a(R"(\b)" + wname + R"(\b(?![.\[(\w])(\s*[+\-*/]\s*))" + nn +
                                R"(\b(?![.\[(\w]))");
                result = std::regex_replace(result, re_a, wname + "." + swizzle + "$1" + nn);
                // N OP W
                std::regex re_b(R"(\b)" + nn + R"(\b(?![.\[(\w])(\s*[+\-*/]\s*))" + wname +
                                R"(\b(?![.\[(\w]))");
                result = std::regex_replace(result, re_b, nn + "$1" + wname + "." + swizzle);
            }
        };
        for (const auto& [wname, wwidth] : wider_varyings) {
            if (wwidth == 4) applyTrunc(wname, vec2_names, "xy");
            applyTrunc(wname, vec3_names, "xyz");
        }

        // Same idea, but for a `vecN(...)` constructor literal as the
        // narrower operand.  Driver: workshop effect 2138904733
        // "Cutout Vignette" (Outset Island 2979524338) does
        //     length(abs(v_TexCoord - vec2(u_offset)) * 1.0)
        // with v_TexCoord as `in vec3` — HLSL truncates to vec2; GLSL
        // errors on `vec3 - vec2`.  Inject `.xy` after `wname` when an
        // arithmetic operator is followed (after optional `(`) by
        // `vec2(`/`vec3(`/`vec4(`.
        auto applyTruncVecCtor = [&](const std::string& wname,
                                     int                 ctorWidth,
                                     const char*         swizzle) {
            const std::string ctor = "vec" + std::to_string(ctorWidth);
            // wname OP <optional whitespace, optional `(`, optional whitespace> vec2(
            std::regex re(R"(\b)" + wname + R"(\b(?=\s*[+\-*/]\s*\(?\s*)" + ctor +
                          R"(\s*\())");
            result = std::regex_replace(result, re, wname + "." + swizzle);
        };
        for (const auto& [wname, wwidth] : wider_varyings) {
            if (wwidth == 4) {
                applyTruncVecCtor(wname, 2, "xy");
                applyTruncVecCtor(wname, 3, "xyz");
            } else if (wwidth == 3) {
                applyTruncVecCtor(wname, 2, "xy");
            }
        }
    }

    // Fix: wider `in` varying used bare in a narrower vec2/vec3 assignment arithmetic.
    // HLSL auto-truncates vec4→vec2 silently; GLSL errors with type mismatch.
    // Scope the rewrite to `vec2 VAR = ...;` / `vec3 VAR = ...;` statements so we
    // only truncate where the target type demands it.  Only rewrite when the bare
    // varying is adjacent to an arithmetic operator (avoids breaking function-call
    // args like `someFunc(v_TexCoord)` where the callee may want the full vec4).
    {
        std::map<std::string, int> varying_widths;
        {
            static const std::regex re_in(R"(\bin\s+vec([234])\s+(\w+)\s*;)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_in);
                 it != std::sregex_iterator();
                 ++it)
                varying_widths[(*it)[2].str()] = std::stoi((*it)[1].str());
        }
        // Skip varyings shadowed by a local float — see comment on the
        // earlier wider-varying transform.  Lens Flare Sun ships
        // `varying vec4 timer; float timer = sin(...);` — bare `timer`
        // resolves to the float local, swizzling it errors.
        {
            std::set<std::string> shadowed;
            static const std::regex re_float(R"(\bfloat\s+(\w+)\s*[=;])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_float);
                 it != std::sregex_iterator(); ++it)
                shadowed.insert((*it)[1].str());
            for (const auto& s : shadowed) varying_widths.erase(s);
        }

        auto processAssign =
            [&](const char* targetType, int targetWidth, const char* targetSwizzle) {
                std::regex re_stmt(std::string(R"(\b)") + targetType + R"(\s+\w+\s*=\s*[^;]*;)");
                regexTransformAll(result, re_stmt, [&](const std::smatch& m) -> std::string {
                    std::string stmt   = m[0].str();
                    size_t      eq_pos = stmt.find('=');
                    if (eq_pos == std::string::npos) return stmt;
                    std::string lhs = stmt.substr(0, eq_pos + 1);
                    std::string rhs = stmt.substr(eq_pos + 1);
                    for (const auto& [vname, vwidth] : varying_widths) {
                        if (vwidth <= targetWidth) continue;
                        std::string rep = vname + "." + targetSwizzle;
                        // VARYING followed by arithmetic op (bare — not already swizzled).
                        std::regex re_after(R"(\b)" + vname + R"(\b(?![.\[(\w])(?=\s*[*+\-/]))");
                        rhs = std::regex_replace(rhs, re_after, rep);
                        // Arithmetic op followed by VARYING (bare).
                        std::regex re_before(R"(([*+\-/]\s*))" + vname + R"(\b(?![.\[(\w]))");
                        rhs = std::regex_replace(rhs, re_before, "$1" + rep);
                    }
                    return lhs + rhs;
                });
            };
        processAssign("vec2", 2, "xy");
        processAssign("vec3", 3, "xyz");
    }

    // Fix: "for (int VAR = -FLOAT_EXPR" → "for (int VAR = int(-FLOAT_EXPR)"
    // HLSL allows implicit float-to-int in for-loop initializers; GLSL does not.
    {
        static const std::regex re(R"(for\s*\(\s*int\s+(\w+)\s*=\s*(-\s*\w+\s*\*\s*\d+))");
        result = std::regex_replace(result, re, "for (int $1 = int($2)");
    }

    // Fix: "for (int VAR = <float_uniform>; …)" — bare-uniform float init.
    // HLSL allows implicit float→int in for-loop initializers; GLSL does not.
    // Driver: workshop spectrum/audio effects with a `uniform float
    // u_MinFreqRange` etc. wired into a for-loop bound, hit on multiple
    // wallpapers in the 2026-05-15 audit (3034862641, 3036962127,
    // 3496072356).  Same idea for the loop-condition comparison.
    {
        std::set<std::string> float_uniforms;
        static const std::regex            re_uf(R"(\buniform\s+float\s+(\w+))");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_uf);
             it != std::sregex_iterator(); ++it)
            float_uniforms.insert((*it)[1].str());
        for (const auto& uf : float_uniforms) {
            // `for (int X = uf` → `for (int X = int(uf)`
            std::regex re_init(R"((for\s*\(\s*int\s+\w+\s*=\s*))" + uf +
                               R"(\b(?![.(\w]))");
            result = std::regex_replace(result, re_init, "$1int(" + uf + ")");
            // `; X </<=/>=/> uf` (loop condition) → wrap rhs in int().  The
            // captured comparator and the LHS variable are kept verbatim.
            std::regex re_cond(R"((;\s*\w+\s*(?:<=|>=|<|>)\s*))" + uf +
                               R"(\b(?![.(\w]))");
            result = std::regex_replace(result, re_cond, "$1int(" + uf + ")");
        }
    }
    // Also fix the loop condition: "VAR <= FLOAT_EXPR * N" → "VAR <= int(FLOAT_EXPR * N)"
    {
        static const std::regex re(R"((\w+)\s*<=\s*(\w+\s*\*\s*\d+)\s*;)");
        // Only apply when the pattern looks like a for-loop condition with a float uniform
        result = std::regex_replace(result, re, "$1 <= int($2);");
    }

    // Fix: scalar assigned to glOutColor → wrap in vec4()
    // HLSL allows implicit float→float4 broadcast on output; GLSL requires explicit vec4().
    // e.g. "glOutColor = sample.x * mask * step(...);" → "glOutColor = vec4(...);"
    // Does NOT match component writes like "glOutColor.a *= ..." (the '.' prevents \s*= match).
    {
        static const std::regex re(R"(\bglOutColor\s*=(?!\s*vec4\s*\()\s*([^;]+);)");
        result = std::regex_replace(result, re, "glOutColor = vec4($1);");
    }

    // Fix: VAR.rgb = mix(VAR, ...) or VAR.xyz = mix(VAR, ...)
    // HLSL lerp() auto-truncates vec4→vec3; GLSL mix() requires matching types.
    // When a vec4 var is swizzled on the LHS but used bare as first arg to mix(), add swizzle.
    {
        static const std::regex re(R"((\w+)\.(rgb|xyz)(\s*=\s*mix\s*\(\s*)\1\b(?!\s*[.\[]))");
        result = std::regex_replace(result, re, "$1.$2$3$1.$2");
    }

    // Fix: "const <type> <funcname>(...)" — remove const qualifier on function return.
    // GLSL disallows qualifiers on function return types; some workshop shaders write
    // "const float fract(float x) { ... }" (shadowing the builtin).  The const is only
    // invalid when followed by '(' (function); const variable decls (const float PI = 3.14;)
    // are left alone because the identifier is followed by '=' instead.
    {
        static const std::regex re(R"(\bconst\s+(vec[234]|float|int|uint|bool)\s+(\w+)\s*\()");
        result = std::regex_replace(result, re, "$1 $2(");
    }

    // Fix: user-defined functions shadowing GLSL builtins.
    // HLSL lets you redefine fract/mod/etc. with your own precision; glslang errors
    // with "overloaded functions must have the same parameter precision qualifiers".
    // When we detect a user definition of a shadowing name, rename the function and
    // all call sites to _w_<name>.  Call sites that were targeting the user function
    // still hit our renamed copy; calls that meant the builtin also get renamed but
    // the user's body is equivalent enough that this is a no-op in practice.
    {
        static const char* const kShadowBuiltins[] = {
            "fract", "mod", "mix", "step", "smoothstep", "clamp", "sign",
        };
        for (const char* bn : kShadowBuiltins) {
            std::regex re_def(std::string(R"(\b(?:vec[234]|float|int|uint|bool)\s+)") + bn +
                              R"(\s*\([^)]*\)\s*\{)");
            if (! std::regex_search(result, re_def)) continue;
            std::string renamed = std::string("_w_") + bn;
            std::regex  re_word(std::string(R"(\b)") + bn + R"(\b)");
            result = std::regex_replace(result, re_word, renamed);
        }
    }

    // Fix: user function expecting vecN called with a wider 'in' varying.
    // HLSL implicitly truncates vec4→vec2 at call sites (e.g. rotateVec2(v_TexCoord, ...)
    // where v_TexCoord is vec4 but rotateVec2 takes vec2); GLSL requires exact match.
    // Collect user function signatures with vec2/vec3 first parameter, then swizzle any
    // bare-varying first argument that's wider than the parameter.
    {
        std::map<std::string, int> func_first_param_dim;
        {
            static const std::regex re_func(
                R"(\b(?:void|float|int|uint|bool|vec[234])\s+(\w+)\s*\(\s*(vec[234])\s+\w+)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_func);
                 it != std::sregex_iterator();
                 ++it) {
                std::string name           = (*it)[1].str();
                std::string type           = (*it)[2].str();
                int         dim            = type == "vec2" ? 2 : type == "vec3" ? 3 : 4;
                func_first_param_dim[name] = dim;
            }
        }
        std::map<std::string, int> varying_dim;
        {
            static const std::regex re_in(R"(\bin\s+vec([234])\s+(\w+)\s*;)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_in);
                 it != std::sregex_iterator();
                 ++it) {
                varying_dim[(*it)[2].str()] = std::stoi((*it)[1].str());
            }
        }
        for (const auto& [fname, expected_dim] : func_first_param_dim) {
            for (const auto& [vname, actual_dim] : varying_dim) {
                if (actual_dim <= expected_dim) continue;
                const char* swiz = expected_dim == 2 ? "xy" : "xyz";
                // Match FNAME( VNAME [,)]) where VNAME is not already swizzled.
                std::regex re(std::string(R"(\b)") + fname + R"(\s*\(\s*)" + vname +
                              R"(\s*([,\)]))");
                result = std::regex_replace(result, re, fname + "(" + vname + "." + swiz + "$1");
            }
        }
    }

    // Fix: HLSL-style brace-list constructors `vec4({1,2,3,4})` are accepted
    // by HLSL but rejected by GLSL ("syntax error, unexpected LEFT_BRACE").
    // Driver: Now Playing (2883312700) ships
    //   vec4 albedo = vec4({ 1.0, 1.0, 1.0, 0.0 });
    // Strip the inner braces so the constructor is `vec4(1.0,1.0,1.0,0.0)`.
    {
        static const std::regex re(R"((vec[234])\s*\(\s*\{([^{}]*)\}\s*\))");
        result = std::regex_replace(result, re, "$1($2)");
    }

    // Fix: HLSL implicit bool→float in arithmetic.  GLSL has no operator for
    // `float * bool` or `bool * float`; the comparison must be wrapped in
    // `float()`.  Driver: Mikey Tokyo Revengers (2622312893) ships
    //   return float(f >= 0) * (f < 1);
    // We detect `<lhs> * ( <bool_expr> )` and `( <bool_expr> ) * <rhs>` at
    // depth 0 and wrap the bool expression in `float(...)`.  Bool expressions
    // are recognised by the presence of `<`, `>`, `<=`, `>=`, `==`, `!=`.
    {
        // Right-hand bool: `* ( ... CMP ... )`
        static const std::regex re1(R"(\*\s*\(\s*([^()]*(?:<=?|>=?|==|!=)[^()]*)\s*\))");
        result = std::regex_replace(result, re1, "* float($1)");
        // Left-hand bool: `( ... CMP ... ) *` — only when not already wrapped
        // in a float(/vec[234]( cast.
        static const std::regex re2(R"(([^A-Za-z0-9_])\(\s*([^()]*(?:<=?|>=?|==|!=)[^()]*)\s*\)\s*\*)");
        result = std::regex_replace(result, re2, "$1float($2) *");
    }

    // Fix: `float NAME = step(scalar, vec_var);` — step propagates the wider
    // arg's dimension, returning a vec.  GLSL refuses vec→float init.
    // Driver: cyberpunk edgerunners (2885492021):
    //   float r = step(1.0, albedo);   // albedo is vec4
    // Append `.x` to the call.  Restricted to `step`/`_westep`: other builtins
    // either accept scalar-broadcast (min/max/clamp) and stay scalar, or have
    // less-clear propagation rules that produce false positives (e.g.
    // `_wemx(_wedot(N, H), 0.0)` returns float despite N,H being vec3).
    {
        // Re-collect vec[234] vars at this point (declarations may have been
        // rewritten by earlier passes).
        auto collectV = [&](const char* type) {
            std::set<std::string> v;
            std::regex re(std::string(R"(\b)") + type + R"(\s+(\w+)\s*[;=,)\[])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                 it != std::sregex_iterator(); ++it)
                v.insert((*it)[1].str());
            return v;
        };
        std::set<std::string> vec_vars;
        for (const char* t : { "vec2", "vec3", "vec4" }) {
            auto s = collectV(t);
            vec_vars.insert(s.begin(), s.end());
        }
        for (const char* fn : { "step", "_westep" }) {
            std::regex re(std::string(R"(\bfloat\s+(\w+)\s*=\s*)") + fn +
                          R"(\s*\(([^()]+),\s*([^()]+)\)\s*;)");
            regexTransformAll(result, re, [&](const std::smatch& m) -> std::string {
                // step's result is vec only when an arg is a BARE vec (no
                // narrowing swizzle).  `v_TexCoord.z` is float, `albedo` is
                // vec4.  Require a vec_var without trailing `.x/.y/.z/.w` etc.,
                // OR a `vec[234](...)` constructor, OR `texture(...)` (vec4).
                auto is_vec_arg = [&](const std::string& a) {
                    if (a.find("texture(") != std::string::npos ||
                        a.find("vec2(") != std::string::npos ||
                        a.find("vec3(") != std::string::npos ||
                        a.find("vec4(") != std::string::npos)
                        return true;
                    for (const auto& v : vec_vars) {
                        std::regex re_v(std::string(R"(\b)") + v + R"(\b(?!\.))");
                        if (std::regex_search(a, re_v)) return true;
                    }
                    return false;
                };
                if (! is_vec_arg(m[2].str()) && ! is_vec_arg(m[3].str()))
                    return m[0].str();
                return std::string("float ") + m[1].str() + " = " + fn + "(" +
                       m[2].str() + ", " + m[3].str() + ").x;";
            });
        }
    }

    // Fix: `const TYPE NAME = EXPR;` where EXPR is not a compile-time
    // constant is rejected by GLSL 330 ("non-constant initializer").  HLSL
    // accepts.  Drivers:
    //   发光少女 (3287715210): `const float cosAngle = cos(radians(u_hueShift));`
    //   cyberpunk edgerunners (2885492021): `const vec4 __vec4 = vec4(albedo.rgb, 0.0);`
    // Drop `const` when the initializer contains:
    //   - A uniform reference (`u_`/`g_`),
    //   - A non-constructor function call (`cos(`, `mix(`, ...),
    //   - Member access (`.<word>`) on a local — almost always non-const.
    // Keep `const` only when the initializer is a literal or a `vec[234](`/
    // `mat[234](`/`float(`/`int(` constructor wrapping literals.
    {
        static const std::regex re(R"(\bconst\s+(float|vec[234]|mat[234])\s+(\w+)\s*=\s*([^;]+);)");
        regexTransformAll(result, re, [&](const std::smatch& m) -> std::string {
            std::string init = m[3].str();
            bool non_const = false;
            if (init.find("u_") != std::string::npos ||
                init.find("g_") != std::string::npos)
                non_const = true;
            if (! non_const) {
                static const std::regex re_dot(R"(\.\s*[A-Za-z_])");
                if (std::regex_search(init, re_dot)) non_const = true;
            }
            if (! non_const) {
                static const std::regex re_fn(R"(\b([A-Za-z_]\w*)\s*\()");
                for (auto it = std::sregex_iterator(init.begin(), init.end(), re_fn);
                     it != std::sregex_iterator(); ++it) {
                    std::string name = (*it)[1].str();
                    if (name == "vec2" || name == "vec3" || name == "vec4" ||
                        name == "mat2" || name == "mat3" || name == "mat4" ||
                        name == "float" || name == "int")
                        continue;
                    non_const = true;
                    break;
                }
            }
            if (! non_const) return m[0].str();
            return m[1].str() + " " + m[2].str() + " = " + init + ";";
        });
    }

    // Fix: `int NAME = step/_westep/etc(...)` — these builtins return float;
    // GLSL refuses float→int implicit narrowing.  Driver: Chill Time
    // (2925278995):
    //   int bar = _westep(1 - shapeCoord.y, barHeight);
    // Re-declare as float; the usage sites (multiplication with float, etc.)
    // accept the wider type without further change.
    {
        static const std::regex re(R"(\bint\s+(\w+)\s*=\s*(step|_westep|smoothstep|mix|min|max|clamp|_wemn|_wemx|abs|sign|floor|ceil|fract|sin|cos|tan|exp|log|sqrt|pow|_wep|mod|atan|asin|acos)\s*\()");
        result = std::regex_replace(result, re, "float $1 = $2(");
    }

    // Fix: `smoothstep(VEC_VAR, VEC_VAR OP SCALAR, SCALAR)` — GLSL refuses
    // mixed-width smoothstep.  HLSL author intent was likely all-scalar.
    // Driver: Mikey Tokyo Revengers (2622312893):
    //   vec2 dist = vec2(length(g.cuv + o));
    //   fragLV += smoothstep(dist, dist + smoothing, thresh);
    // dist was `vec2(scalar)` — both components equal.  Narrow first two args
    // to `.x` so smoothstep(float, float, float) → float.
    {
        // Collect vec[234] vars from declarations (any scope).
        auto collectV2 = [&](const char* type) {
            std::set<std::string> v;
            std::regex re(std::string(R"(\b)") + type +
                          R"(\s+(\w+)\s*[;=,)\[])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                 it != std::sregex_iterator(); ++it)
                v.insert((*it)[1].str());
            return v;
        };
        std::set<std::string> vec_vars;
        for (const char* t : { "vec2", "vec3", "vec4" }) {
            auto s = collectV2(t);
            vec_vars.insert(s.begin(), s.end());
        }
        if (! vec_vars.empty()) {
            static const std::regex re(R"(\bsmoothstep\s*\(\s*(\w+)\s*,\s*(\w+)\s*([+\-*/])\s*(\w+)\s*,\s*(\w+)\s*\))");
            regexTransformAll(result, re, [&](const std::smatch& m) -> std::string {
                std::string a1 = m[1].str();
                std::string a2 = m[2].str();
                if (a1 != a2 || ! vec_vars.count(a1)) return m[0].str();
                return std::string("smoothstep(") + a1 + ".x, " + a2 + ".x " +
                       m[3].str() + " " + m[4].str() + ", " + m[5].str() + ")";
            });
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// TranslateHlslClip
// ---------------------------------------------------------------------------
// HLSL's `clip(expr);` is a statement that discards the fragment when expr is
// negative.  The macro form `#define clip(x) if ((x) < 0.0) discard` breaks on
// the trailing `;` left behind by the source — semicolons inside macro
// expansion become orphan empty statements in some contexts.  A string
// rewrite using findMatchingParen + skipWhitespaceAndSemicolon (the same
// balanced-paren recipe TranslateGeometryShader uses for OUT.Append) is the
// correct shape: it eats the `;` that follows the closing `)` and emits an
// if-discard statement directly.
//
// Only matches whole identifiers (`\bclip\s*\(`) so substrings like `clipped`,
// `flipclamp`, or `aclipb` are left untouched.
inline std::string TranslateHlslClip(const std::string& src) {
    static const std::regex re(R"(\bclip\s*\()");
    std::string result;
    result.reserve(src.size());

    size_t pos = 0;
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re);
         it != std::sregex_iterator();
         ++it) {
        const auto& m = *it;
        // Append the inter-match preamble unchanged.
        result.append(src, pos, (size_t)m.position() - pos);

        // The match captures `clip` plus optional whitespace plus `(`.  The
        // opening paren is the last char of the match.
        size_t openParen  = (size_t)m.position() + m.length() - 1;
        size_t afterClose = findMatchingParen(src, openParen);
        if (afterClose == std::string::npos) {
            // Unmatched — copy through verbatim and advance past the match.
            result.append(src, (size_t)m.position(), m.length());
            pos = (size_t)m.position() + m.length();
            continue;
        }
        size_t innerBeg = openParen + 1;
        size_t innerEnd = afterClose - 1; // position of ')'
        size_t after    = skipWhitespaceAndSemicolon(src, afterClose);

        std::string arg = src.substr(innerBeg, innerEnd - innerBeg);
        result.append("if ((");
        result.append(arg);
        result.append(") < 0.0) discard;");
        pos = after;
    }
    result.append(src, pos, std::string::npos);
    return result;
}

// ---------------------------------------------------------------------------
// FixFragmentGlPosition
// ---------------------------------------------------------------------------
// HLSL pixel shaders receive SV_Position which maps to window-space coords.
// The WE cross-compiler sometimes emits `gl_Position` for this — but in GLSL,
// gl_Position is a vertex-stage-only output, undeclared in fragment shaders.
// The semantic equivalent is gl_FragCoord.  Replace unqualified gl_Position
// references.  Caller must only invoke this on fragment-stage shader source.
inline std::string FixFragmentGlPosition(const std::string& src) {
    static const std::regex re(R"(\bgl_Position\b)");
    return std::regex_replace(src, re, "gl_FragCoord");
}

// ---------------------------------------------------------------------------
// StripStrayEndifs
// ---------------------------------------------------------------------------
// Some workshop shaders have stray #endif directives with no matching #if.
// glslang's preprocessor truncates all output after a stray #endif, causing
// "unexpected end of file" errors.  Windows HLSL preprocessor is more lenient.
// This pass removes #endif lines that would bring the nesting depth below zero.
inline std::string StripStrayEndifs(const std::string& src) {
    std::string result;
    result.reserve(src.size());
    int         depth = 0;
    std::size_t pos   = 0;

    while (pos < src.size()) {
        auto lineEnd = src.find('\n', pos);
        if (lineEnd == std::string::npos) lineEnd = src.size();

        std::string_view line(src.data() + pos, lineEnd - pos);

        // Find first non-whitespace character
        auto first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos && line[first] == '#') {
            auto directive = line.substr(first + 1);
            auto dstart    = directive.find_first_not_of(" \t");
            if (dstart != std::string_view::npos) {
                directive = directive.substr(dstart);
                if (directive.starts_with("if")) {
                    ++depth;
                } else if (directive.starts_with("endif")) {
                    if (depth > 0) {
                        --depth;
                    } else {
                        // Stray #endif — skip this line
                        pos = (lineEnd < src.size()) ? lineEnd + 1 : lineEnd;
                        continue;
                    }
                } else if (directive.starts_with("else") || directive.starts_with("elif")) {
                    if (depth <= 0) {
                        // Stray #else/#elif — skip
                        pos = (lineEnd < src.size()) ? lineEnd + 1 : lineEnd;
                        continue;
                    }
                }
            }
        }

        result.append(src, pos, lineEnd - pos);
        if (lineEnd < src.size()) result.push_back('\n');
        pos = (lineEnd < src.size()) ? lineEnd + 1 : lineEnd;
    }

    return result;
}

// ---------------------------------------------------------------------------
// FixEffectAlpha
// ---------------------------------------------------------------------------
// Fix effect shaders that write glOutColor component-wise (.r/.g/.b/.rgb) without
// ever writing .a — the alpha channel is left at 0 (undefined), making the output
// invisible under Translucent blend (src*alpha + dst*(1-alpha)).
// Injects alpha preservation from the input texture (g_Texture0) before the closing
// brace of main().  Only fires for shaders with component writes but NO full
// assignment (glOutColor = ...) and NO explicit alpha write (.a / swizzle with a/w).
inline std::string FixEffectAlpha(const std::string& src) {
    // Check for full assignment: glOutColor = ...  (not component write like .rgb = ...)
    static const std::regex re_full(R"(\bglOutColor\s*=)");
    if (std::regex_search(src, re_full)) return src;

    // Check for any component write (.r, .g, .b, .rgb, etc.)
    static const std::regex re_comp(R"(\bglOutColor\s*\.)");
    if (! std::regex_search(src, re_comp)) return src;

    // Check for explicit alpha write (.a, .rgba, .xyzw, any swizzle containing a or w)
    static const std::regex re_alpha(R"(\bglOutColor\s*\.\s*\w*[aw])");
    if (std::regex_search(src, re_alpha)) return src;

    // Find last '}' (closing brace of main()) and inject alpha preservation
    auto pos = src.rfind('}');
    if (pos == std::string::npos) return src;

    std::string result = src;
    result.insert(pos, "    glOutColor.a = texSample2D(g_Texture0, v_TexCoord.xy).a;\n");

#ifndef WP_SUPPRESS_DEBUG_LOGGING
    // Dump the fixed shader for diagnostics
    static std::atomic<int> dump_idx { 0 };
    int                     idx = dump_idx.fetch_add(1);
    if (idx < 10) {
        std::string   path = "/tmp/alpha_fix_dump_" + std::to_string(idx) + ".glsl";
        std::ofstream f(path);
        if (f) f << result;
        LOG_INFO("FixEffectAlpha[%d]: injected alpha preservation → %s (%zu bytes)",
                 idx,
                 path.c_str(),
                 result.size());
    }
#endif

    return result;
}

// ---------------------------------------------------------------------------
// FixCombineAlpha
// ---------------------------------------------------------------------------
// Fix combine shaders (shine_combine, godrays_combine) that add effect alpha
// to the original alpha:  `albedo.a = saturate(albedo.a + rays.a);`
// This extends the alpha boundary beyond the original object, making
// previously-invisible pixels partially visible.  When composited with
// Translucent blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), these extra-alpha
// pixels create a ghosting fringe at puppet/sprite edges.
//
// Fix: remove the additive alpha line so the variable retains its original
// alpha from the input texture (g_Texture1).  For fullscreen layers this is
// a no-op (original alpha is always 1, so saturate(1+x) == 1 anyway).
//
// Only matches `VAR.a = saturate(VAR.a + OTHER.a);` where the SAME variable
// appears on both sides — this avoids touching fluidsimulation_combine which
// uses `albedo.a = saturate(prev.a + albedo.a)` (different LHS/first operand).
// ClampAudioReactiveShift
//
// WE's `chromatic_aberration` effect (and similar shake/displace shaders)
// multiplies a per-channel UV offset by `v_AudioShift`, a varying computed in
// the vertex shader from `g_AudioSpectrum16Left/Right`.  The varying is
// `smoothstep(bounds) * g_AudioMultiply`, nominally [0, 1+].  When the
// effect is STACKED (id=210 on wallpaper 2866203962 applies two CA passes
// back-to-back on a ping-pong pair), each pass's sampling offset reads from
// the previous pass's already-shifted output.  With loud sustained audio the
// cumulative R–B separation reaches 5-10% of the texture and shreds text
// layers into illegible RGB fragments a few seconds after scene load.
//
// Cap the effective shift to a modest fraction by wrapping every
// `<u_Offset_uniform> * v_AudioShift` term into
// `<u_Offset_uniform> * min(v_AudioShift, 0.25)`.  0.25 keeps the effect
// visible on isolated passes but prevents pathological compounding across
// stacked passes.  Logged once per process so repeat hits don't spam.
//
// Future direction — see [pass-dump.md] "Not" note: the architecturally
// purer fix is to rebind the ORIGINAL base texture to the second+ CA pass
// (extra texture slot + graph change) so channels sample from a pristine
// source each pass.  That's a render-graph refactor, not a shader tweak.
inline std::string ClampAudioReactiveShift(const std::string& src) {
    // Pattern: (u_<WORD>Offset * v_AudioShift) — also tolerate whitespace
    // and optional parentheses.  Capture the offset uniform so we emit the
    // minimum rewrite.
    static const std::regex re(R"(\b(u_\w*[Oo]ffset)\s*\*\s*v_AudioShift\b)");
    if (! std::regex_search(src, re)) return src;

    // Cap at 1.0 — not a real clamp (softSat in AudioAnalyzer already peaks
    // well below 1.0), but prevents unbounded values from bugs in our audio
    // pipeline.  Previously we used 0.25 but that muted authored CA stacking
    // on wallpaper 2866203962 so the signature yellow/cyan split barely
    // appeared.  The AUDIOPROCESSING=0 text-shift bug it was originally
    // guarding against is now addressed upstream in WPSceneParser (combo
    // override for chromatic_aberration).
    std::string result = std::regex_replace(src, re, "$1 * min(v_AudioShift, 1.0)");

    // Log only the first time in the process — diagnostic so we can see
    // this fire on new wallpapers without flooding the journal.
    static bool s_logged = false;
    if (! s_logged) {
        s_logged = true;
        LOG_INFO("ClampAudioReactiveShift: applied (v_AudioShift capped at 1.0 "
                 "where multiplied by u_*Offset).  See pass-dump.md.");
    }
    return result;
}

inline std::string FixCombineAlpha(const std::string& src) {
    // After macro expansion, `saturate(x)` becomes `(clamp(x, 0.0, 1.0))`.
    // Match: VAR.a = (clamp(VAR.a + OTHER.a, 0.0, 1.0));
    static const std::regex re(
        R"((\w+)\.a\s*=\s*\(clamp\(\s*\1\.a\s*\+\s*\w+\.a\s*,\s*0\.0\s*,\s*1\.0\s*\)\)\s*;)");
    if (! std::regex_search(src, re)) return src;

    std::string result = std::regex_replace(
        src, re, "// $& // removed: bloom alpha must not extend object boundary");

    LOG_INFO("FixCombineAlpha: removed additive alpha in combine shader");
    return result;
}
