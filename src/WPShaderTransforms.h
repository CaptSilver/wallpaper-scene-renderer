#pragma once
// Pure string→string shader transform functions extracted from WPShaderParser.cpp.
// Header-only; depends only on the C++ standard library (+Logging.h for FixEffectAlpha diagnostics).

#include <atomic>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "Utils/Logging.h"

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
        std::regex re(R"(\b(v_WorldPos|v_WorldRight|v_ScreenCoord)\s*=\s*(mul\s*\([^;]+?\))\s*;(?!\s*\.xyz))");
        result = std::regex_replace(result, re, "$1 = ($2).xyz;");
    }

    // 9. Replace OUT.Append(expr); → expr; EmitVertex();
    //    Handles both OUT.Append(v); and OUT.Append(FuncCall(...));
    //    Uses balanced-paren matching for nested calls.
    {
        const std::string marker = "OUT.Append(";
        std::string replaced;
        size_t pos = 0;
        while (true) {
            size_t start = result.find(marker, pos);
            if (start == std::string::npos) {
                replaced.append(result, pos, std::string::npos);
                break;
            }
            replaced.append(result, pos, start - pos);
            size_t inner = start + marker.size();
            int    depth = 1;
            size_t i     = inner;
            for (; i < result.size() && depth > 0; i++) {
                if (result[i] == '(') depth++;
                else if (result[i] == ')') depth--;
            }
            // i now points past the matching ')'
            // Skip optional trailing whitespace and semicolon
            size_t after = i;
            while (after < result.size() && (result[after] == ' ' || result[after] == '\t'))
                after++;
            if (after < result.size() && result[after] == ';') after++;

            std::string innerExpr = result.substr(inner, i - 1 - inner);
            // If inner expr is just a bare identifier (the old PS_INPUT var),
            // skip it — the output varyings are already set directly.
            // Only keep the expr if it's a function call or complex expression.
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
            std::regex re(R"(\b(?:void|float|vec[234]|int|uint|bool|mat[234])\s+(\w+)\s*\(([^)]*)\))");
            std::regex paramRe(R"((?:in\s+|out\s+|inout\s+|const\s+)*(\w+)\s+\w+)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                 it != std::sregex_iterator(); ++it) {
                std::string paramStr = (*it)[2].str();
                std::vector<std::string> types;
                for (auto pit = std::sregex_iterator(paramStr.begin(), paramStr.end(), paramRe);
                     pit != std::sregex_iterator(); ++pit)
                    types.push_back((*pit)[1].str());
                funcSigs[(*it)[1].str()] = types;
            }
        }

        const std::string gl_pos = "gl_in[0].gl_Position";
        std::string out;
        size_t pos = 0;
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

            // Find enclosing function call: scan backward for '(' at depth 0
            bool needsTrunc = false;
            size_t scan = found;
            int depth = 0;
            while (scan > 0) {
                scan--;
                if (result[scan] == ')') depth++;
                else if (result[scan] == '(') {
                    if (depth == 0) {
                        // Count commas between '(' and gl_pos at depth 0 → arg index
                        int argIdx = 0, cd = 0;
                        for (size_t j = scan + 1; j < found; j++) {
                            if (result[j] == '(') cd++;
                            else if (result[j] == ')') cd--;
                            else if (result[j] == ',' && cd == 0) argIdx++;
                        }
                        // Extract function name before '('
                        size_t ne = scan;
                        while (ne > 0 && std::isspace(result[ne - 1])) ne--;
                        size_t ns = ne;
                        while (ns > 0 && (std::isalnum(result[ns - 1]) || result[ns - 1] == '_'))
                            ns--;
                        std::string fn = result.substr(ns, ne - ns);
                        if (funcSigs.count(fn)) {
                            auto& p = funcSigs[fn];
                            if (argIdx < (int)p.size() && p[argIdx] == "vec3")
                                needsTrunc = true;
                        }
                        break;
                    }
                    depth--;
                }
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
        std::regex re(R"((\bfloat\s+)(\w+)(\s*=\s*int\s*\())");
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
            std::regex re(R"(\buint\s+(\w+)\s*=\s*\((\w+)\s*([\+\-])\s*(\d+)\)\s*%\s*(\d+\b))");
            result = std::regex_replace(result, re, "uint $1 = uint((int($2) $3 $4) % $5)");
        }
        // Special case: uint VAR = EXPR % N;  →  uint VAR = uint(int(EXPR) % N);
        {
            std::regex re(R"(\buint\s+(\w+)\s*=\s*\b(\w+)\s*%\s*(\d+\b))");
            result = std::regex_replace(result, re, "uint $1 = uint(int($2) % $3)");
        }
        // General case: EXPR % N  →  int(EXPR) % N
        {
            std::regex re(R"(\b(\w+)\s*%\s*(\d+\b))");
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
                                             const char* oob_pattern,
                                             const char* bare_swizzle) {
            std::vector<std::string> to_upgrade;
            std::regex               re_decl(std::string(R"(\b)") + small_type + R"(\s+(\w+)\s*;)");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re_decl);
                 it != std::sregex_iterator();
                 ++it) {
                std::string name = (*it)[1].str();
                if (std::regex_search(result, std::regex(R"(\b)" + name + oob_pattern)))
                    to_upgrade.push_back(std::move(name));
            }
            for (const auto& name : to_upgrade) {
                // Upgrade the declaration (vec2 → vec4, etc.)
                std::regex re(std::string(R"(\b)") + small_type + R"((\s+)" + name + R"(\s*;))");
                result = std::regex_replace(result, re, std::string(big_type) + "$1");
                // After upgrading, bare uses of this variable as a texture() coordinate
                // are now too wide (e.g. texture(sampler2D, vec4) is invalid).
                // Add a swizzle to bring it back to the original size.
                // Only matches bare NAME with no following dot (i.e. no existing swizzle).
                std::regex re_tex(R"(\btexture\s*\(\s*(\w+)\s*,\s*)" + name + R"(\s*\))");
                result = std::regex_replace(
                    result, re_tex, "texture($1, " + name + "." + bare_swizzle + ")");
            }
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
            std::set<std::string> vars;
            std::regex            re(std::string(R"(\b)") + type + R"(\s+(\w+)\b)");
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
            for (const auto& d : dst) {
                for (const auto& s : src) {
                    if (d == s) continue;
                    std::regex re("\\b(" + d + ")\\s*=\\s*(" + s + ")\\s*;");
                    result =
                        std::regex_replace(result, re, "$1 = $2." + std::string(swizzle) + ";");
                }
            }
        };

        fixTrunc(vec2_vars, vec3_vars, "xy");
        fixTrunc(vec2_vars, vec4_vars, "xy");
        fixTrunc(vec3_vars, vec4_vars, "xyz");

        // Fix: HLSL implicit vector truncation in arithmetic expressions.
        // HLSL allows vec2 + vec4 (truncates the larger); GLSL requires matching
        // dimensions.  When a known vecN variable is in arithmetic with a swizzle
        // of more than N components, truncate the swizzle to N.
        auto fixArithSwizzleTrunc = [&result](const std::set<std::string>& small_vars,
                                              int target_width) {
            std::string swiz_chars = "xyzwrgbastpq";
            for (const auto& v : small_vars) {
                // VAR OP WORD.XXXX  where XXXX has more than target_width components
                for (int sw = 4; sw > target_width; --sw) {
                    std::regex re("\\b(" + v + ")(\\s*[+\\-*/]\\s*)(\\w+)\\.([" +
                                  swiz_chars + "]{" + std::to_string(sw) + "})\\b");
                    std::string tmp;
                    size_t      lastPos = 0;
                    bool        found   = false;
                    for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                         it != std::sregex_iterator();
                         ++it) {
                        found              = true;
                        std::string swizzle = (*it)[4].str().substr(0, target_width);
                        tmp.append(result, lastPos, (size_t)(*it).position() - lastPos);
                        tmp += (*it)[1].str() + (*it)[2].str() + (*it)[3].str() + "." + swizzle;
                        lastPos = (size_t)(*it).position() + (*it).length();
                    }
                    if (found) {
                        tmp.append(result, lastPos, std::string::npos);
                        result = std::move(tmp);
                    }

                    // Reverse: WORD.XXXX OP VAR
                    std::regex re2("(\\w+)\\.([" + swiz_chars + "]{" + std::to_string(sw) +
                                   "})(\\s*[+\\-*/]\\s*)\\b(" + v + ")\\b");
                    tmp.clear();
                    lastPos = 0;
                    found   = false;
                    for (auto it = std::sregex_iterator(result.begin(), result.end(), re2);
                         it != std::sregex_iterator();
                         ++it) {
                        found              = true;
                        std::string swizzle = (*it)[2].str().substr(0, target_width);
                        tmp.append(result, lastPos, (size_t)(*it).position() - lastPos);
                        tmp += (*it)[1].str() + "." + swizzle + (*it)[3].str() + (*it)[4].str();
                        lastPos = (size_t)(*it).position() + (*it).length();
                    }
                    if (found) {
                        tmp.append(result, lastPos, std::string::npos);
                        result = std::move(tmp);
                    }
                }
            }
        };
        // For arithmetic truncation, only consider local variable declarations
        // (TYPE NAME ; or TYPE NAME =), not function parameters (TYPE NAME , or TYPE NAME )).
        // This avoids false positives when the same name appears as vec2 param + vec4 local.
        auto collectLocal = [&result](const char* type) {
            std::set<std::string> vars;
            std::regex            re(std::string(R"(\b)") + type + R"(\s+(\w+)\s*[;=])");
            for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                 it != std::sregex_iterator();
                 ++it)
                vars.insert((*it)[1].str());
            return vars;
        };
        const auto local_vec2 = collectLocal("vec2");
        const auto local_vec3 = collectLocal("vec3");
        fixArithSwizzleTrunc(local_vec2, 2);
        fixArithSwizzleTrunc(local_vec3, 3);

        // Fix: HLSL implicit vector truncation — larger variable OP smaller swizzle.
        // Instead of truncating the variable (which cascades mismatches downstream),
        // expand the swizzle to match the variable width by repeating its last char.
        // E.g. vec3_var - expr.xx → vec3_var - expr.xxx
        auto fixArithSwizzleExpand = [&result](const std::set<std::string>& large_vars,
                                               int var_width, int swizzle_width) {
            std::string swiz_chars = "xyzwrgbastpq";
            int         pad_count  = var_width - swizzle_width;
            for (const auto& v : large_vars) {
                // VAR OP WORD.XX  (swizzle_width components, smaller than var_width)
                std::regex re("\\b(" + v + ")(\\s*[+\\-*/]\\s*)(\\w+)\\.([" +
                              swiz_chars + "]{" + std::to_string(swizzle_width) + "})\\b");
                std::string tmp;
                size_t      lastPos = 0;
                bool        found   = false;
                for (auto it = std::sregex_iterator(result.begin(), result.end(), re);
                     it != std::sregex_iterator();
                     ++it) {
                    // Skip if variable already has a swizzle
                    size_t vEnd = (size_t)(*it).position() + (*it)[1].length();
                    if (vEnd < result.size() && result[vEnd] == '.') continue;
                    found = true;
                    tmp.append(result, lastPos, (size_t)(*it).position() - lastPos);
                    std::string swiz = (*it)[4].str();
                    char        last = swiz.back();
                    for (int j = 0; j < pad_count; j++) swiz += last;
                    tmp += (*it)[1].str() + (*it)[2].str() +
                           (*it)[3].str() + "." + swiz;
                    lastPos = (size_t)(*it).position() + (*it).length();
                }
                if (found) {
                    tmp.append(result, lastPos, std::string::npos);
                    result = std::move(tmp);
                }

                // Reverse: WORD.XX OP VAR
                std::regex re2("(\\w+)\\.([" + swiz_chars + "]{" +
                               std::to_string(swizzle_width) + "})(\\s*[+\\-*/]\\s*)\\b(" +
                               v + ")\\b");
                tmp.clear();
                lastPos = 0;
                found   = false;
                for (auto it = std::sregex_iterator(result.begin(), result.end(), re2);
                     it != std::sregex_iterator();
                     ++it) {
                    size_t vStart = (size_t)(*it).position(4);
                    size_t vEnd   = vStart + (*it)[4].length();
                    if (vEnd < result.size() && result[vEnd] == '.') continue;
                    found = true;
                    tmp.append(result, lastPos, (size_t)(*it).position() - lastPos);
                    std::string swiz = (*it)[2].str();
                    char        last = swiz.back();
                    for (int j = 0; j < pad_count; j++) swiz += last;
                    tmp += (*it)[1].str() + "." + swiz + (*it)[3].str() +
                           (*it)[4].str();
                    lastPos = (size_t)(*it).position() + (*it).length();
                }
                if (found) {
                    tmp.append(result, lastPos, std::string::npos);
                    result = std::move(tmp);
                }
            }
        };
        const auto local_vec4 = collectLocal("vec4");
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
        std::regex re(R"(\bvec3\s+(\w+)\s*=\s*(\([^;]*\)\s*\*\s*\(vec4\s*\([^;]*?\)\)\s*)\s*;)");
        result = std::regex_replace(result, re, "vec3 $1 = ($2).xyz;");
    }

    // Fix: HLSL pow(scalar, vecN) broadcasts the scalar; GLSL requires matching genType.
    // When the pow() result is used inside a vecN() constructor, move the broadcast inside:
    //   vecN(pow(X, Y)) → pow(vecN(X), vecN(Y))
    // Wrapping an already-vecN arg in vecN() is a safe copy-constructor identity.
    // Only handles one level of nesting inside each pow argument (sufficient in practice).
    {
        std::regex re(R"(\b(vec[234])\s*\(\s*pow\s*\()"
                      R"(([^(),]*(?:\([^)]*\)[^(),]*)*),\s*)"
                      R"(([^()]*(?:\([^)]*\)[^()]*)*)\)\s*\))");
        result = std::regex_replace(result, re, "pow($1($2), $1($3))");
    }

    // Fix: "float VAR = texture(SAMPLER, COORD);" → add .x swizzle
    // HLSL implicitly converts vec4 texture result to float (first component);
    // GLSL requires an explicit swizzle.
    {
        std::regex re(R"(\bfloat\s+(\w+)\s*=\s*(texture\w*\s*\([^;]*?\))\s*;)");
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
        std::regex re_decl(R"(\bvec4\s+(\w+)\s*=\s*texture\w*\s*\()");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_decl);
             it != std::sregex_iterator();
             ++it)
            tex_vec4_vars.insert((*it)[1].str());
        // Collect known float uniforms
        std::set<std::string> float_vars;
        std::regex re_float(R"(\buniform\s+float\s+(\w+)\s*;)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_float);
             it != std::sregex_iterator();
             ++it)
            float_vars.insert((*it)[1].str());
        // Also add local float vars
        std::regex re_flocal(R"(\bfloat\s+(\w+)\s*=)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_flocal);
             it != std::sregex_iterator();
             ++it)
            float_vars.insert((*it)[1].str());
        // For each tex vec4 var, check if it appears in arithmetic with float vars
        for (const auto& tvar : tex_vec4_vars) {
            bool used_as_scalar = false;
            for (const auto& fvar : float_vars) {
                // float * vec4_var  or  vec4_var * float
                std::regex re_arith("\\b" + fvar + R"(\s*\*\s*)" + tvar + R"((?!\s*[\.\[]))" +
                                    "|\\b" + tvar + R"((?!\s*[\.\[]))" + R"(\s*\*\s*)" + fvar +
                                    "\\b");
                if (std::regex_search(result, re_arith)) {
                    used_as_scalar = true;
                    break;
                }
            }
            if (used_as_scalar) {
                // Replace bare uses of tvar (not followed by . or [) with tvar.x
                // But preserve declarations: "vec4 tvar = texture" and "in vec4 tvar;"
                std::string mark = "__DECL_MARK_" + tvar;
                // Mark local declaration
                result = std::regex_replace(
                    result, std::regex(R"(\bvec4\s+)" + tvar + R"(\s*=\s*texture)"),
                    "vec4 " + mark + " = texture");
                // Mark 'in' declaration
                result = std::regex_replace(
                    result, std::regex(R"(\bin\s+vec4\s+)" + tvar + R"(\s*;)"),
                    "in vec4 " + mark + ";");
                // Replace bare uses with .x
                std::regex re_bare("\\b" + tvar + "(?!\\s*[.\\[\\w])");
                result = std::regex_replace(result, re_bare, tvar + ".x");
                // Restore marks
                result = std::regex_replace(result, std::regex(mark), tvar);
            }
        }
    }

    // Fix: integer literal as ternary condition → bool()
    // HLSL allows int in ternary condition; GLSL requires bool.
    // Match bare integer after = ( , that is followed by ? (ternary operator).
    {
        std::regex re(R"(([\=\(,]\s*)(\d+)(\s*\?))");
        result = std::regex_replace(result, re, "$1bool($2)$3");
    }

    // Fix: (expr CMP expr) in arithmetic → float(expr CMP expr)
    // HLSL allows bool in arithmetic (true=1.0, false=0.0); GLSL does not.
    // Matches parenthesized comparison followed by arithmetic operator.
    {
        std::regex re(R"(\(([^()]*(?:<=|>=|==|!=|<|>)[^()]*)\)(\s*[*+/\-]))");
        result = std::regex_replace(result, re, "float($1)$2");
    }

    // Fix: "float_var *= bool_var" → "float_var *= float(bool_var)"
    // HLSL allows bool in arithmetic (true=1.0, false=0.0); GLSL does not.
    // Collect all local bool variable names, then wrap them with float() when used
    // in compound assignment (*=, +=, -=, /=) or after arithmetic operators (* + - /).
    {
        std::set<std::string> bool_vars;
        std::regex            re_bool(R"(\bbool\s+(\w+)\s*[=;])");
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
        std::regex re(R"(\bint\s+(\w+)\s*=\s*(step\s*\([^;]*\))\s*;)");
        result = std::regex_replace(result, re, "float $1 = $2;");
    }

    // Fix: texture(sampler2D, VEC4_VAR) → texture(sampler2D, VEC4_VAR.xy)
    // HLSL varyings may be declared as vec4 (float4 semantics) and used bare as texture
    // coordinates. sampler2D texture() requires vec2.  Add .xy for any vec4/vec3 varying
    // used bare (no existing swizzle) as a texture() coordinate.
    {
        // Collect all "in vec4" and "in vec3" varying names
        std::set<std::string> wide_varyings;
        std::regex            re_in(R"(\bin\s+vec[34]\s+(\w+)\s*;)");
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
        std::regex re(R"(\bconst\s+(vec[234]|float|int)\s+(\w+)\s*=\s*(texture\w*\s*\())");
        result = std::regex_replace(result, re, "$1 $2 = $3");
    }

    // Fix: writing to 'in' varying — HLSL allows mutable inputs; GLSL doesn't.
    // Create a mutable copy for any 'in' variable that has compound assignment (+=,-=,*=,/=).
    {
        std::regex re_in(R"(\bin\s+(vec[234]|float)\s+(\w+)\s*;)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_in);
             it != std::sregex_iterator();
             ++it) {
            std::string type = (*it)[1].str();
            std::string name = (*it)[2].str();
            std::regex  re_compound("\\b" + name + R"(\s*[\+\-\*\/]=)");
            if (! std::regex_search(result, re_compound)) continue;

            // Rename all body uses: NAME → _m_NAME, then fix the 'in' declaration back
            std::string mut = "_m_" + name;
            result = std::regex_replace(result, std::regex("\\b" + name + "\\b"), mut);
            // Restore the 'in' declaration
            result = std::regex_replace(
                result, std::regex("\\bin\\s+" + type + "\\s+" + mut + "\\s*;"),
                "in " + type + " " + name + ";");
            // Add mutable copy at start of main()
            result = std::regex_replace(
                result, std::regex(R"(void\s+main\s*\(\s*\)\s*\{)"),
                "void main() {\n " + type + " " + mut + " = " + name + ";");
            break; // handle one at a time to avoid iterator invalidation
        }
    }

    // Fix: "vec3 VAR = vec4(EXPR)" → "vec3 VAR = vec4(EXPR).xyz"
    // HLSL implicit truncation from vec4 constructor result to vec3.
    {
        std::regex re(R"(\bvec3\s+(\w+)\s*=\s*(vec4\s*\([^;]*?\))\s*;)");
        result = std::regex_replace(result, re, "vec3 $1 = $2.xyz;");
    }

    // Fix: "float VAR = VEC_EXPR" → "float VAR = (VEC_EXPR).x"
    // HLSL implicitly takes the first component; GLSL requires explicit swizzle.
    // Detect known vec2/vec3/vec4 variables used in a float assignment.
    {
        std::set<std::string> vec_vars;
        std::regex re_vec(R"(\b(?:uniform|in)\s+vec[234]\s+(\w+)\s*;)");
        for (auto it = std::sregex_iterator(result.begin(), result.end(), re_vec);
             it != std::sregex_iterator();
             ++it)
            vec_vars.insert((*it)[1].str());
        for (const auto& name : vec_vars) {
            // float VAR = VEC_NAME * EXPR;  →  float VAR = VEC_NAME.x * EXPR;
            std::regex re(R"(\bfloat\s+(\w+)\s*=\s*)" + name + R"(\s*([*+\-/]))");
            result = std::regex_replace(result, re, "float $1 = " + name + ".x $2");
            // float VAR = EXPR * VEC_NAME;  →  float VAR = EXPR * VEC_NAME.x;
            std::regex re2(R"(([*+\-/])\s*)" + name + R"(\s*;)");
            result = std::regex_replace(result, re2, "$1 " + name + ".x;");
        }
    }

    // Fix: "for (int VAR = -FLOAT_EXPR" → "for (int VAR = int(-FLOAT_EXPR)"
    // HLSL allows implicit float-to-int in for-loop initializers; GLSL does not.
    {
        std::regex re(R"(for\s*\(\s*int\s+(\w+)\s*=\s*(-\s*\w+\s*\*\s*\d+))");
        result = std::regex_replace(result, re, "for (int $1 = int($2)");
    }
    // Also fix the loop condition: "VAR <= FLOAT_EXPR * N" → "VAR <= int(FLOAT_EXPR * N)"
    {
        std::regex re(R"((\w+)\s*<=\s*(\w+\s*\*\s*\d+)\s*;)");
        // Only apply when the pattern looks like a for-loop condition with a float uniform
        result = std::regex_replace(result, re, "$1 <= int($2);");
    }

    // Fix: scalar assigned to glOutColor → wrap in vec4()
    // HLSL allows implicit float→float4 broadcast on output; GLSL requires explicit vec4().
    // e.g. "glOutColor = sample.x * mask * step(...);" → "glOutColor = vec4(...);"
    // Does NOT match component writes like "glOutColor.a *= ..." (the '.' prevents \s*= match).
    {
        std::regex re(R"(\bglOutColor\s*=(?!\s*vec4\s*\()\s*([^;]+);)");
        result = std::regex_replace(result, re, "glOutColor = vec4($1);");
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

    // Dump the fixed shader for diagnostics
    static std::atomic<int> dump_idx { 0 };
    int idx = dump_idx.fetch_add(1);
    if (idx < 10) {
        std::string path = "/tmp/alpha_fix_dump_" + std::to_string(idx) + ".glsl";
        std::ofstream f(path);
        if (f) f << result;
        LOG_INFO("FixEffectAlpha[%d]: injected alpha preservation → %s (%zu bytes)",
                 idx, path.c_str(), result.size());
    }

    return result;
}
