#include "WPShaderParser.hpp"

#include "Fs/IBinaryStream.h"
#include "Utils/Logging.h"
#include "WPJson.hpp"

#include "wpscene/WPUniform.h"
#include "Fs/VFS.h"
#include "Utils/Sha.hpp"
#include "Utils/String.h"
#include "WPCommon.hpp"

#include "Vulkan/ShaderComp.hpp"

#include <regex>
#include <stack>
#include <charconv>
#include <string>
#include <fstream>
#include <sstream>
#include <future>
#include <mutex>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <set>

static constexpr std::string_view SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__" };

#define SHADER_DIR    "spvs01"
#define SHADER_SUFFIX "spvs"

using namespace wallpaper;

namespace
{

static constexpr const char* pre_shader_code = R"(#version 330
#define GLSL 1
#define HLSL 0
#define highp

#define CAST2(x) (vec2(x))
#define CAST3(x) (vec3(x))
#define CAST4(x) (vec4(x))
#define CAST3X3(x) (mat3(x))

#define texSample2D texture
#define texSample2DLod textureLod
#define mul(x, y) ((y) * (x))
#define frac fract
#define atan2 atan
#define fmod(x, y) (x-y*trunc(x/y))
#define ddx dFdx
#define ddy(x) dFdy(-(x))
#define saturate(x) (clamp(x, 0.0, 1.0))
#define log10(x) (log(x) / log(10.0))

// HLSL built-ins broadcast scalar to vector; GLSL requires matching genType.
// Overloads must be defined BEFORE the #define so their bodies call the
// real built-in, while all subsequent shader code gets redirected.
vec2 _wep(vec2 x, float y) { return pow(x, vec2(y)); }
vec3 _wep(vec3 x, float y) { return pow(x, vec3(y)); }
vec4 _wep(vec4 x, float y) { return pow(x, vec4(y)); }
vec2 _wep(float x, vec2 y) { return pow(vec2(x), y); }
vec3 _wep(float x, vec3 y) { return pow(vec3(x), y); }
vec4 _wep(float x, vec4 y) { return pow(vec4(x), y); }
float _wep(float x, float y) { return pow(x, y); }
vec2 _wep(vec2 x, vec2 y) { return pow(x, y); }
vec3 _wep(vec3 x, vec3 y) { return pow(x, y); }
vec4 _wep(vec4 x, vec4 y) { return pow(x, y); }
#define pow _wep
vec2 _wemx(float x, vec2 y) { return max(vec2(x), y); }
vec3 _wemx(float x, vec3 y) { return max(vec3(x), y); }
vec4 _wemx(float x, vec4 y) { return max(vec4(x), y); }
vec2 _wemx(vec2 x, float y) { return max(x, vec2(y)); }
vec3 _wemx(vec3 x, float y) { return max(x, vec3(y)); }
vec4 _wemx(vec4 x, float y) { return max(x, vec4(y)); }
float _wemx(float x, float y) { return max(x, y); }
vec2 _wemx(vec2 x, vec2 y) { return max(x, y); }
vec3 _wemx(vec3 x, vec3 y) { return max(x, y); }
vec4 _wemx(vec4 x, vec4 y) { return max(x, y); }
#define max _wemx
vec2 _wemn(float x, vec2 y) { return min(vec2(x), y); }
vec3 _wemn(float x, vec3 y) { return min(vec3(x), y); }
vec4 _wemn(float x, vec4 y) { return min(vec4(x), y); }
vec2 _wemn(vec2 x, float y) { return min(x, vec2(y)); }
vec3 _wemn(vec3 x, float y) { return min(x, vec3(y)); }
vec4 _wemn(vec4 x, float y) { return min(x, vec4(y)); }
float _wemn(float x, float y) { return min(x, y); }
vec2 _wemn(vec2 x, vec2 y) { return min(x, y); }
vec3 _wemn(vec3 x, vec3 y) { return min(x, y); }
vec4 _wemn(vec4 x, vec4 y) { return min(x, y); }
#define min _wemn
float _wedot(vec4 x, vec3 y) { return dot(x.xyz, y); }
float _wedot(vec3 x, vec4 y) { return dot(x, y.xyz); }
float _wedot(vec4 x, vec2 y) { return dot(x.xy, y); }
float _wedot(vec2 x, vec4 y) { return dot(x, y.xy); }
float _wedot(vec3 x, vec2 y) { return dot(x.xy, y); }
float _wedot(vec2 x, vec3 y) { return dot(x, y.xy); }
float _wedot(vec2 x, vec2 y) { return dot(x, y); }
float _wedot(vec3 x, vec3 y) { return dot(x, y); }
float _wedot(vec4 x, vec4 y) { return dot(x, y); }
#define dot _wedot

#define float1 float
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define lerp mix

__SHADER_PLACEHOLD__

)";

static constexpr const char* pre_shader_code_vert = R"(
#define attribute in
#define varying out

)";
static constexpr const char* pre_shader_code_frag = R"(
#define varying in
#define gl_FragColor glOutColor
out vec4 glOutColor;

)";
// Geometry shader: no attribute/varying defines needed.
// Layout declarations are injected by TranslateGeometryShader based on [maxvertexcount].
static constexpr const char* pre_shader_code_geom = R"(
)";

inline std::string LoadGlslInclude(fs::VFS& vfs, const std::string& input) {
    std::string::size_type pos = 0;
    std::string            output;
    std::string::size_type linePos = std::string::npos;

    while (linePos = input.find("#include", pos), linePos != std::string::npos) {
        auto lineEnd  = input.find_first_of('\n', linePos);
        auto lineSize = lineEnd - linePos;
        auto lineStr  = input.substr(linePos, lineSize);
        output.append(input.substr(pos, linePos - pos));

        auto inP         = lineStr.find_first_of('\"') + 1;
        auto inE         = lineStr.find_last_of('\"');
        auto includeName = lineStr.substr(inP, inE - inP);
        auto includeSrc  = fs::GetFileContent(vfs, "/assets/shaders/" + includeName);
        output.append("\n//-----include " + includeName + "\n");
        output.append(LoadGlslInclude(vfs, includeSrc));
        output.append("\n//-----include end\n");

        pos = lineEnd;
    }
    output.append(input.substr(pos));
    return output;
}

inline void ParseWPShader(const std::string& src, WPShaderInfo* pWPShaderInfo,
                          const std::vector<WPShaderTexInfo>& texinfos) {
    auto& combos       = pWPShaderInfo->combos;
    auto& wpAliasDict  = pWPShaderInfo->alias;
    auto& shadervalues = pWPShaderInfo->svs;
    auto& defTexs      = pWPShaderInfo->defTexs;
    idx   texcount     = std::ssize(texinfos);

    // pos start of line
    std::string::size_type pos = 0, lineEnd = std::string::npos;
    while ((lineEnd = src.find_first_of(('\n'), pos)), true) {
        const auto clineEnd = lineEnd;
        const auto line     = src.substr(pos, lineEnd - pos);

        /*
        if(line.find("attribute ") != std::string::npos || line.find("in ") != std::string::npos) {
            update_pos = true;
        }
        */
        if (line.find("// [COMBO]") != std::string::npos) {
            nlohmann::json combo_json;
            if (PARSE_JSON(line.substr(line.find_first_of('{')), combo_json)) {
                if (combo_json.contains("combo")) {
                    std::string name;
                    int32_t     value = 0;
                    GET_JSON_NAME_VALUE(combo_json, "combo", name);
                    GET_JSON_NAME_VALUE(combo_json, "default", value);
                    combos[name] = std::to_string(value);
                }
            }
        } else if (line.find("uniform ") != std::string::npos) {
            if (line.find("// {") != std::string::npos) {
                nlohmann::json sv_json;
                if (PARSE_JSON(line.substr(line.find_first_of('{')), sv_json)) {
                    std::vector<std::string> defines =
                        utils::SpliteString(line.substr(0, line.find_first_of(';')), ' ');

                    std::string material;
                    GET_JSON_NAME_VALUE_NOWARN(sv_json, "material", material);
                    if (! material.empty()) wpAliasDict[material] = defines.back();

                    ShaderValue sv;
                    std::string name  = defines.back();
                    bool        istex = name.compare(0, 9, "g_Texture") == 0;
                    if (istex) {
                        wpscene::WPUniformTex wput;
                        wput.FromJson(sv_json);
                        i32 index { 0 };
                        STRTONUM(name.substr(9), index);
                        if (! wput.default_.empty()) defTexs.push_back({ index, wput.default_ });
                        if (! wput.combo.empty()) {
                            if (index >= texcount)
                                combos[wput.combo] = "0";
                            else
                                combos[wput.combo] = "1";
                        }
                        // formatcombo: auto-generate combo from the format field name
                        // e.g. {"format":"normalmap","formatcombo":true} → NORMALMAP=1
                        if (sv_json.contains("formatcombo") &&
                            sv_json.at("formatcombo").get<bool>()) {
                            std::string format;
                            GET_JSON_NAME_VALUE_NOWARN(sv_json, "format", format);
                            if (! format.empty()) {
                                std::string comboName = format;
                                std::transform(comboName.begin(),
                                               comboName.end(),
                                               comboName.begin(),
                                               ::toupper);
                                combos[comboName] = (index < texcount) ? "1" : "0";
                            }
                        }
                        if (index < texcount && texinfos[(usize)index].enabled) {
                            auto& compos = texinfos[(usize)index].composEnabled;

                            usize num = std::min(std::size(compos), std::size(wput.components));
                            for (usize i = 0; i < num; i++) {
                                if (compos[i]) combos[wput.components[i].combo] = "1";
                            }
                        }

                    } else {
                        if (sv_json.contains("default")) {
                            auto        value = sv_json.at("default");
                            ShaderValue sv;
                            name = defines.back();
                            if (value.is_string()) {
                                std::vector<float> v;
                                GET_JSON_VALUE(value, v);
                                sv = std::span<const float>(v);
                            } else if (value.is_number()) {
                                sv.setSize(1);
                                GET_JSON_VALUE(value, sv[0]);
                            }
                            shadervalues[name] = sv;
                        }
                        if (sv_json.contains("combo")) {
                            std::string name;
                            GET_JSON_NAME_VALUE(sv_json, "combo", name);
                            combos[name] = "1";
                        }
                    }
                    if (defines.back()[0] != 'g') {
                        LOG_INFO("user shader uniform: '%s' (alias: '%s')",
                                 defines.back().c_str(),
                                 material.c_str());
                    }
                }
            }
        }

        // end
        if (line.find("void main()") != std::string::npos || clineEnd == std::string::npos) {
            break;
        }
        pos = lineEnd + 1;
    }
}

inline usize FindIncludeInsertPos(const std::string& src, usize startPos) {
    /* rule:
    after attribute/varying/uniform/struct
    befor any func
    not in {}
    not in #if #endif
    */
    (void)startPos;

    auto NposToZero = [](usize p) {
        return p == std::string::npos ? 0 : p;
    };
    auto search = [](const std::string& p, usize pos, const auto& re) {
        auto        startpos = p.begin() + (isize)pos;
        std::smatch match;
        if (startpos < p.end() && std::regex_search(startpos, p.end(), match, re)) {
            return pos + (usize)match.position();
        }
        return std::string::npos;
    };
    auto searchLast = [](const std::string& p, const auto& re) {
        auto        startpos = p.begin();
        std::smatch match;
        while (startpos < p.end() && std::regex_search(startpos, p.end(), match, re)) {
            startpos++;
            startpos += match.position();
        }
        return startpos >= p.end() ? std::string::npos : usize(startpos - p.begin());
    };
    auto nextLinePos = [](const std::string& p, usize pos) {
        return p.find_first_of('\n', pos) + 1;
    };

    usize mainPos  = src.find("void main(");
    bool  two_main = src.find("void main(", mainPos + 2) != std::string::npos;
    if (two_main) return 0;

    usize pos;
    {
        const std::regex reAfters(R"(\n(attribute|varying|uniform|struct) )");
        usize            afterPos = searchLast(src, reAfters);
        if (afterPos != std::string::npos) {
            afterPos = nextLinePos(src, afterPos + 1);
        }
        pos = std::min({ NposToZero(afterPos), mainPos });
    }
    {
        std::stack<usize> ifStack;
        usize             nowPos { 0 };
        const std::regex  reIfs(R"((#if|#endif))");
        while (true) {
            auto p = search(src, nowPos + 1, reIfs);
            if (p > mainPos || p == std::string::npos) break;
            if (src.substr(p, 3) == "#if") {
                ifStack.push(p);
            } else {
                if (ifStack.empty()) break;
                usize ifp = ifStack.top();
                ifStack.pop();
                usize endp = p;
                if (pos > ifp && pos <= endp) {
                    pos = nextLinePos(src, endp + 1);
                }
            }
            nowPos = p;
        }
        pos = std::min({ pos, mainPos });
    }

    return NposToZero(pos);
}

// Translate Wallpaper Engine geometry shader syntax to valid GLSL 330.
// WE uses an HLSL-inspired dialect: IN[0].field, OUT.Append(v), PS_INPUT,
// [maxvertexcount(N)].  This function converts to standard GLSL geometry
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

inline EShLanguage ToGLSL(ShaderType type) {
    switch (type) {
    case ShaderType::VERTEX: return EShLangVertex;
    case ShaderType::GEOMETRY: return EShLangGeometry;
    case ShaderType::FRAGMENT: return EShLangFragment;
    default: return EShLangVertex;
    }
}

inline std::string Preprocessor(const std::string& in_src, ShaderType type, const Combos& combos,
                                WPPreprocessorInfo& process_info) {
    std::string res;

    std::string src = wallpaper::WPShaderParser::PreShaderHeader(in_src, combos, type);

    // Handle #require directive: inject required function implementations
    {
        // LightingV1: PBR lighting using g_LightsPosition/g_LightsColorRadius uniforms
        // that our backend provides.  Uses ComputePBRLightShadow from common_pbr_2.h.
        static const std::string lightingV1_impl =
            "\n"
            "uniform vec4 g_LightsPosition[4];\n"
            "uniform vec4 g_LightsColorRadius[4];\n"
            "\n"
            "vec3 PerformLighting_V1(vec3 worldPos, vec3 albedo, vec3 normal,\n"
            "    vec3 viewVector, vec3 specularTint, vec3 f0, float roughness, float metallic)\n"
            "{\n"
            "    vec3 light = vec3(0.0);\n"
            "    for (int i = 0; i < 4; ++i) {\n"
            "        vec3 lightDelta = g_LightsPosition[i].xyz - worldPos;\n"
            "        float radius = g_LightsColorRadius[i].w;\n"
            "        if (radius <= 0.0) continue;\n"
            "        light += ComputePBRLightShadow(normal, lightDelta, viewVector,\n"
            "            albedo, g_LightsColorRadius[i].rgb, radius, 1.0,\n"
            "            specularTint, f0, roughness, metallic, 1.0);\n"
            "    }\n"
            "    return light;\n"
            "}\n";

        std::regex  re_require("(^|\r?\n)#require (.+)(\r?\n)");
        std::smatch m;
        std::string tmp = src;
        std::string result;
        while (std::regex_search(tmp, m, re_require)) {
            result += m.prefix().str() + m[1].str();
            std::string req_name = m[2].str();
            // Trim whitespace
            while (! req_name.empty() && (req_name.back() == ' ' || req_name.back() == '\r'))
                req_name.pop_back();
            if (req_name == "LightingV1") {
                result += lightingV1_impl;
            } else {
                result += "//#require " + req_name;
            }
            result += m[3].str();
            tmp = m.suffix().str();
        }
        result += tmp;
        src = result;
    }

    glslang::TShader::ForbidIncluder includer;
    glslang::TShader                 shader(ToGLSL(type));
    const EShMessages emsg { (EShMessages)(EShMsgDefault | EShMsgSpvRules | EShMsgRelaxedErrors |
                                           EShMsgSuppressWarnings | EShMsgVulkanRules) };

    auto* data = src.c_str();
    shader.setStrings(&data, 1);
    shader.preprocess(&vulkan::DefaultTBuiltInResource,
                      110,
                      EProfile::ECoreProfile,
                      false,
                      false,
                      emsg,
                      &res,
                      includer);

    // (?:^|\s) lets us match "out vec2 foo;" at column 0 (no qualifier) as well as
    // "smooth out vec4 foo;" where the qualifier precedes the keyword.
    // The optional (\[\])? handles geometry shader array inputs (e.g., "in vec4 v_Color[];").
    std::regex re_io(R"((?:^|\s)(in|out)\s[\s\w]+\s(\w+)\s*(?:\[\])?\s*;)",
                     std::regex::ECMAScript | std::regex::multiline);
    for (auto it = std::sregex_iterator(res.begin(), res.end(), re_io);
         it != std::sregex_iterator();
         it++) {
        std::smatch mc = *it;
        if (mc[1] == "in") {
            process_info.input[mc[2]] = mc[0].str();
        } else {
            process_info.output[mc[2]] = mc[0].str();
        }
    }

    std::regex re_tex(R"(uniform\s+sampler2D\s+g_Texture(\d+))", std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(res.begin(), res.end(), re_tex);
         it != std::sregex_iterator();
         it++) {
        std::smatch mc  = *it;
        auto        str = mc[1].str();
        uint        slot;
        auto [ptr, ec] { std::from_chars(str.c_str(), str.c_str() + str.size(), slot) };
        if (ec != std::errc()) continue;
        process_info.active_tex_slots.insert(slot);
    }
    return res;
}

// Check if a GLSL I/O declaration has an integer type that requires flat interpolation.
// Per Vulkan spec, integer/double fragment inputs must be decorated "flat".
inline bool NeedsFlatDecoration(const std::string& decl) {
    // Match integer types: int, uint, ivec2-4, uvec2-4
    static const std::regex re_int_type(R"(\b(int|uint|ivec[234]|uvec[234])\b)");
    // Already has flat qualifier
    static const std::regex re_has_flat(R"(\bflat\b)");
    return std::regex_search(decl, re_int_type) && ! std::regex_search(decl, re_has_flat);
}

inline std::string Finalprocessor(const WPShaderUnit& unit, const WPPreprocessorInfo* pre,
                                  const WPPreprocessorInfo* next) {
    std::string insert_str {};
    auto&       cur    = unit.preprocess_info;
    bool        is_geo = (unit.stage == ShaderType::GEOMETRY);
    if (pre != nullptr) {
        for (auto& [k, v] : pre->output) {
            // For geometry shaders, check for gs_in_ prefixed name
            bool already_declared =
                exists(cur.input, k) || (is_geo && exists(cur.input, "gs_in_" + k));
            if (! already_declared) {
                auto n = std::regex_replace(v, std::regex(R"(\s*out\s)"), " in ");
                if (NeedsFlatDecoration(n)) n = "flat " + n;
                // Geometry shader inputs: unsized arrays with gs_in_ prefix
                if (is_geo) {
                    n = std::regex_replace(n, std::regex(R"(\bin\s+([\w]+)\s+(v_\w+))"),
                                           "in $1 gs_in_$2");
                    n = std::regex_replace(n, std::regex(R"((\w)\s*;)"), "$1[];");
                }
                insert_str += n + '\n';
            }
        }
    }
    if (next != nullptr) {
        for (auto& [k, v] : next->input) {
            // For geometry shader inputs with gs_in_ prefix,
            // check canonical name (without prefix) against current outputs
            std::string canon = k;
            if (canon.substr(0, 6) == "gs_in_")
                canon = canon.substr(6);
            if (! exists(cur.output, k) && ! exists(cur.output, canon)) {
                auto n = std::regex_replace(v, std::regex(R"(\s*in\s)"), " out ");
                if (NeedsFlatDecoration(n)) n = "flat " + n;
                // Strip gs_in_ prefix and [] suffix for non-geometry outputs
                if (k.substr(0, 6) == "gs_in_") {
                    n = std::regex_replace(n, std::regex(R"(\bgs_in_(v_\w+))"), "$1");
                    n = std::regex_replace(n, std::regex(R"(\[\]\s*;)"), ";");
                }
                insert_str += n + '\n';
            }
        }
    }
    std::regex re_hold(SHADER_PLACEHOLD.data());

    // LOG_INFO("insert: %s", insert_str.c_str());
    // return std::regex_replace(
    //    std::regex_replace(cur.result, re_hold, insert_str), std::regex(R"(\s+\n)"), "\n");
    return std::regex_replace(unit.src, re_hold, insert_str);
}

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
        std::regex re(R"(\bglOutColor\s*=\s*(?!vec4\s*\()([^;]+);)");
        result = std::regex_replace(result, re, "glOutColor = vec4($1);");
    }

    return result;
}

inline std::string GenSha1(std::span<const WPShaderUnit> units) {
    std::string shas;
    for (auto& unit : units) {
        shas += utils::genSha1(unit.src);
    }
    return utils::genSha1(shas);
}
inline std::string GetCachePath(std::string_view scene_id, std::string_view filename) {
    return std::string("/cache/") + std::string(scene_id) + "/" SHADER_DIR "/" +
           std::string(filename) + "." SHADER_SUFFIX;
}

inline bool LoadShaderFromFile(std::vector<ShaderCode>& codes, fs::IBinaryStream& file) {
    codes.clear();
    i32 ver = ReadSPVVesion(file);

    usize count = file.ReadUint32();
    assert(count <= 16 && count >= 0);
    if (count > 16) return false;

    codes.resize(count);
    for (usize i = 0; i < count; i++) {
        auto& c = codes[i];

        u32 size = file.ReadUint32();
        assert(size % 4 == 0);
        if (size % 4 != 0) return false;

        c.resize(size / 4);
        file.Read((char*)c.data(), size);
    }
    return true;
}

inline void SaveShaderToFile(std::span<const ShaderCode> codes, fs::IBinaryStreamW& file) {
    char nop[256] { '\0' };

    WriteSPVVesion(file, 1);
    file.WriteUint32((u32)codes.size());
    for (const auto& c : codes) {
        u32 size = (u32)c.size() * 4;
        file.WriteUint32(size);
        file.Write((const char*)c.data(), size);
    }
    file.Write(nop, sizeof(nop));
}

// Standalone compile function: runs Finalprocessor + FixImplicitConversions + glslang.
// Thread-safe — each call operates on its own copy of units.
static bool CompileShaderUnits(std::vector<WPShaderUnit>& units, std::vector<ShaderCode>& codes) {
    std::vector<vulkan::ShaderCompUnit> vunits(units.size());
    for (usize i = 0; i < units.size(); i++) {
        auto&               unit     = units[i];
        auto&               vunit    = vunits[i];
        WPPreprocessorInfo* pre_info = i >= 1 ? &units[i - 1].preprocess_info : nullptr;
        WPPreprocessorInfo* post_info =
            i + 1 < units.size() ? &units[i + 1].preprocess_info : nullptr;

        unit.src = Finalprocessor(unit, pre_info, post_info);
        unit.src = FixImplicitConversions(unit.src);

        vunit.src   = unit.src;
        vunit.stage = ToGLSL(unit.stage);
    }

    // Fix cross-stage varying type mismatches
    if (units.size() >= 2) {
        auto parseVaryings = [](const std::string& src, const char* kw) {
            std::map<std::string, std::string> m;
            std::regex re(std::string(R"(\b)") + kw +
                          R"(\s+(vec[234]|float)\s+(\w+)\s*(\[\])?\s*;)");
            for (auto it = std::sregex_iterator(src.begin(), src.end(), re);
                 it != std::sregex_iterator();
                 ++it)
                m[(*it)[2].str()] = (*it)[1].str();
            return m;
        };
        auto dim = [](const std::string& t) -> int {
            if (t == "float") return 1;
            if (t == "vec2") return 2;
            if (t == "vec3") return 3;
            if (t == "vec4") return 4;
            return 0;
        };
        auto upgradeVarying = [&dim](std::string&       src,
                                     const char*        io_kw,
                                     const std::string& name,
                                     const std::string& old_type,
                                     const std::string& new_type) {
            std::regex re_decl(std::string("\\b") + io_kw + "\\s+" + old_type + "\\s+" + name +
                               R"(\s*(\[\])?\s*;)");
            std::string suffix = "$1";
            src = std::regex_replace(
                src, re_decl, std::string(io_kw) + " " + new_type + " " + name + suffix + ";");

            if (std::string(io_kw) == "out") {
                int         od = dim(old_type), nd = dim(new_type);
                std::string pad;
                for (int p = 0; p < nd - od; p++) pad += ", 0.0";
                std::regex re_assign("\\b" + name + "(\\s*=\\s*)((?!" + new_type +
                                     "\\s*\\()[^;]+);");
                src = std::regex_replace(
                    src, re_assign, name + "$1" + new_type + "($2" + pad + ");");
            }
        };

        for (usize pair = 0; pair + 1 < units.size(); pair++) {
            auto outVars     = parseVaryings(units[pair].src, "out");
            auto inVars      = parseVaryings(units[pair + 1].src, "in");
            bool out_changed = false, in_changed = false;

            for (auto& [name, itype] : inVars) {
                auto it = outVars.find(name);
                if (it == outVars.end() || it->second == itype) continue;
                int od = dim(it->second), id = dim(itype);
                if (od == 0 || id == 0) continue;

                if (id > od) {
                    upgradeVarying(units[pair].src, "out", name, it->second, itype);
                    out_changed = true;
                } else {
                    upgradeVarying(units[pair + 1].src, "in", name, itype, it->second);
                    in_changed = true;
                }
            }
            if (out_changed) vunits[pair].src = units[pair].src;
            if (in_changed) vunits[pair + 1].src = units[pair + 1].src;
        }
    }

    vulkan::ShaderCompOpt opt;
    opt.client_ver             = glslang::EShTargetVulkan_1_1;
    opt.auto_map_bindings      = true;
    opt.auto_map_locations     = true;
    opt.relaxed_errors_glsl    = true;
    opt.relaxed_rules_vulkan   = true;
    opt.suppress_warnings_glsl = true;

    // Geometry shader: rename VS outputs to match GS input names
    {
        bool has_gs = false;
        for (auto& u : units) {
            if (u.stage == ShaderType::GEOMETRY) { has_gs = true; break; }
        }
        if (has_gs) {
            std::vector<std::pair<std::string, std::string>> gs_renames;
            for (auto& unit : units) {
                if (unit.stage != ShaderType::GEOMETRY) continue;
                std::regex re_gs_in(R"(\bin\s+(?:flat\s+)?(?:vec[234]|float|int|ivec[234])\s+(gs_in_(\w+))\s*\[\])");
                for (auto it = std::sregex_iterator(unit.src.begin(), unit.src.end(), re_gs_in);
                     it != std::sregex_iterator(); ++it) {
                    gs_renames.push_back({ (*it)[2].str(), (*it)[1].str() });
                }
            }

            for (auto& unit : units) {
                if (unit.stage != ShaderType::VERTEX) continue;
                for (auto& [canon, gs_name] : gs_renames) {
                    std::regex re_decl("\\bout\\s+((?:flat\\s+)?(?:vec[234]|float|int|ivec[234])\\s+)" +
                                       canon + "\\s*;");
                    unit.src = std::regex_replace(unit.src, re_decl, "out $1" + gs_name + ";");
                    std::regex re_use("\\b" + canon + "\\b");
                    unit.src = std::regex_replace(unit.src, re_use, gs_name);
                }
            }

            for (usize i = 0; i < units.size(); i++) {
                vunits[i].src = units[i].src;
            }

            // Dump GS shaders for debugging (thread-safe counter)
            {
                static std::atomic<int> dump_idx { 0 };
                int idx = dump_idx.fetch_add(1);
                if (idx < 5) {
                    const char* stage_names[] = { "VERTEX", "GEOMETRY", "FRAGMENT" };
                    for (usize i = 0; i < vunits.size(); i++) {
                        int si = (vunits[i].stage == EShLangVertex)     ? 0
                                 : (vunits[i].stage == EShLangGeometry) ? 1
                                                                        : 2;
                        std::string path = "/tmp/gs_dump_" +
                                           std::to_string(idx) + "_" +
                                           std::string(stage_names[si]) + ".glsl";
                        std::ofstream f(path);
                        if (f) f << vunits[i].src;
                        LOG_INFO("GS dump[%d]: stage=%s → %s (%zu bytes)",
                                 idx, stage_names[si], path.c_str(),
                                 vunits[i].src.size());
                    }
                }
            }
        }
    }

    std::vector<vulkan::Uni_ShaderSpv> spvs(units.size());

    if (! vulkan::CompileAndLinkShaderUnits(vunits, opt, spvs)) {
        return false;
    }

    codes.clear();
    for (auto& spv : spvs) {
        codes.emplace_back(std::move(spv->spirv));
    }
    return true;
}

// Deferred parallel compilation state
struct PendingShaderCompilation {
    std::string              sha1;
    std::string              cache_path;
    std::vector<ShaderCode>* output;
};
static std::mutex s_compileMtx;
static std::unordered_map<std::string, std::shared_future<std::vector<ShaderCode>>> s_asyncCompilations;
static std::vector<PendingShaderCompilation> s_pendingOutputs;

} // namespace

std::string WPShaderParser::PreShaderSrc(fs::VFS& vfs, const std::string& src,
                                         WPShaderInfo*                       pWPShaderInfo,
                                         const std::vector<WPShaderTexInfo>& texinfos) {
    std::string            newsrc(src);
    std::string::size_type pos = 0;
    std::string            include;
    while (pos = src.find("#include", pos), pos != std::string::npos) {
        auto begin = pos;
        pos        = src.find_first_of('\n', pos);
        newsrc.replace(begin, pos - begin, pos - begin, ' ');
        include.append(src.substr(begin, pos - begin) + "\n");
    }
    include = LoadGlslInclude(vfs, include);

    ParseWPShader(include, pWPShaderInfo, texinfos);
    ParseWPShader(newsrc, pWPShaderInfo, texinfos);

    newsrc.insert(FindIncludeInsertPos(newsrc, 0), include);
    return newsrc;
}

std::string WPShaderParser::PreShaderHeader(const std::string& src, const Combos& combos,
                                            ShaderType type) {
    std::string pre(pre_shader_code);
    if (type == ShaderType::VERTEX) pre += pre_shader_code_vert;
    if (type == ShaderType::GEOMETRY) pre += pre_shader_code_geom;
    if (type == ShaderType::FRAGMENT) pre += pre_shader_code_frag;
    std::string header(pre);
    for (const auto& c : combos) {
        std::string cup(c.first);
        std::transform(c.first.begin(), c.first.end(), cup.begin(), ::toupper);
        if (c.second.empty()) {
            LOG_ERROR("combo '%s' can't be empty", cup.c_str());
            continue;
        }
        header.append("#define " + cup + " " + c.second + "\n");
    }
    return header + src;
}

void WPShaderParser::InitGlslang() { glslang::InitializeProcess(); }
void WPShaderParser::FinalGlslang() { glslang::FinalizeProcess(); }

bool WPShaderParser::CompileToSpv(std::string_view scene_id, std::span<WPShaderUnit> units,
                                  std::vector<ShaderCode>& codes, fs::VFS& vfs,
                                  WPShaderInfo*                    shader_info,
                                  std::span<const WPShaderTexInfo> texs) {
    (void)texs;

    // Translate WE geometry shader syntax to GLSL before preprocessing.
    for (auto& unit : units) {
        if (unit.stage == ShaderType::GEOMETRY) {
            unit.src = TranslateGeometryShader(unit.src);
        }
    }

    std::for_each(units.begin(), units.end(), [shader_info](auto& unit) {
        unit.src = Preprocessor(unit.src, unit.stage, shader_info->combos, unit.preprocess_info);
    });

    // Post-preprocessing: inject GS layout declarations with evaluated max_vertices.
    for (auto& unit : units) {
        if (unit.stage != ShaderType::GEOMETRY) continue;

        int maxVerts = 4;
        auto it = shader_info->combos.find("TRAILSUBDIVISION");
        if (it != shader_info->combos.end()) {
            int subdiv = std::stoi(it->second);
            maxVerts = 4 + subdiv * 2;
        }

        std::string layouts = "layout(points) in;\n"
                              "layout(triangle_strip, max_vertices = " +
                              std::to_string(maxVerts) + ") out;\n\n";
        auto ver_pos = unit.src.find("#version");
        if (ver_pos != std::string::npos) {
            auto eol = unit.src.find('\n', ver_pos);
            if (eol != std::string::npos)
                unit.src.insert(eol + 1, layouts);
            else
                unit.src += "\n" + layouts;
        } else {
            unit.src = layouts + unit.src;
        }
    }

    bool has_cache_dir = vfs.IsMounted("cache");

    if (has_cache_dir) {
        std::string sha1            = GenSha1(units);
        std::string cache_file_path = GetCachePath(scene_id, sha1);

        if (vfs.Contains(cache_file_path)) {
            // Disk cache hit — load synchronously
            auto cache_file = vfs.Open(cache_file_path);
            if (! cache_file || ! ::LoadShaderFromFile(codes, *cache_file)) {
                LOG_ERROR("load shader from \'%s\' failed", cache_file_path.c_str());
                return false;
            }
            return true;
        }

        // Cache miss — launch async compilation (deferred until FlushPendingCompilations)
        {
            std::lock_guard<std::mutex> lock(s_compileMtx);
            if (s_asyncCompilations.find(sha1) == s_asyncCompilations.end()) {
                // First time seeing this SHA1 — copy units and launch async
                auto units_copy = std::vector<WPShaderUnit>(units.begin(), units.end());
                auto future = std::async(std::launch::async,
                    [u = std::move(units_copy)]() mutable {
                        std::vector<ShaderCode> result;
                        CompileShaderUnits(u, result);
                        return result;
                    });
                s_asyncCompilations[sha1] = future.share();
            }
            s_pendingOutputs.push_back({sha1, cache_file_path, &codes});
        }
        return true;

    } else {
        // No cache dir — compile synchronously
        auto units_vec = std::vector<WPShaderUnit>(units.begin(), units.end());
        return CompileShaderUnits(units_vec, codes);
    }
}

void WPShaderParser::FlushPendingCompilations(fs::VFS& vfs) {
    std::lock_guard<std::mutex> lock(s_compileMtx);

    if (s_pendingOutputs.empty()) return;

    auto t0 = std::chrono::steady_clock::now();

    LOG_INFO("Flushing %zu deferred shader compilations (%zu unique)...",
             s_pendingOutputs.size(), s_asyncCompilations.size());

    std::set<std::string> saved_to_disk;
    for (auto& pending : s_pendingOutputs) {
        auto it = s_asyncCompilations.find(pending.sha1);
        if (it == s_asyncCompilations.end()) continue;

        const auto& compiled = it->second.get(); // blocks until compilation finishes
        if (compiled.empty()) {
            LOG_ERROR("async shader compilation failed for %s", pending.sha1.c_str());
            continue;
        }
        *pending.output = compiled;

        // Write to disk cache (once per unique SHA1)
        if (saved_to_disk.find(pending.sha1) == saved_to_disk.end()) {
            if (auto cache_file = vfs.OpenW(pending.cache_path); cache_file) {
                ::SaveShaderToFile(compiled, *cache_file);
            }
            saved_to_disk.insert(pending.sha1);
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    LOG_INFO("Shader compilation complete: %zu unique shaders in %lld ms",
             saved_to_disk.size(), (long long)ms);

    s_pendingOutputs.clear();
    s_asyncCompilations.clear();
}
