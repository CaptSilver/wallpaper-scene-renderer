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

#include "WPShaderTransforms.h"

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

// TranslateGeometryShader — moved to WPShaderTransforms.h

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
            "// Forward declaration — definition comes from common_pbr_2.h (included later)\n"
            "vec3 ComputePBRLightShadow(vec3 N, vec3 L, vec3 V, vec3 albedo, vec3 lightColor,\n"
            "    float radius, float exponent, vec3 specularTint, vec3 baseReflectance,\n"
            "    float roughness, float metallic, float shadowFactor);\n"
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

// NeedsFlatDecoration — moved to WPShaderTransforms.h

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

// FixImplicitConversions — moved to WPShaderTransforms.h
// FixEffectAlpha — moved to WPShaderTransforms.h

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
        if (unit.stage == ShaderType::FRAGMENT) {
            unit.src = FixCombineAlpha(unit.src);
            unit.src = FixEffectAlpha(unit.src);
        }

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
