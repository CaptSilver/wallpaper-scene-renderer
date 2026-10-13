#include <doctest.h>

#include "ShaderSourceScan.hpp"

#include <random>
#include <regex>
#include <string>
#include <utility>
#include <vector>

using namespace wallpaper;

// The reference implementations below are the regex walks WPShaderParser used
// before ShaderSourceScan.hpp, verbatim.  Every scan must agree with them on
// every input, since their results decide where includes are pasted and which
// stage inputs/outputs and texture slots a shader is linked with.
namespace
{
std::size_t RefIfOrEndif(const std::string& p, std::size_t pos) {
    const std::regex re(R"((#if|#endif))");
    auto             startpos = p.begin() + (std::ptrdiff_t)pos;
    std::smatch      match;
    if (startpos < p.end() && std::regex_search(startpos, p.end(), match, re)) {
        return pos + (std::size_t)match.position();
    }
    return std::string::npos;
}

std::size_t RefLastDeclarationLine(const std::string& p) {
    const std::regex re(R"(\n(attribute|varying|uniform) )");
    auto             startpos = p.begin();
    std::smatch      match;
    while (startpos < p.end() && std::regex_search(startpos, p.end(), match, re)) {
        startpos++;
        startpos += match.position();
    }
    return startpos >= p.end() ? std::string::npos : std::size_t(startpos - p.begin());
}

using Matches = std::vector<std::pair<std::size_t, std::string>>;

Matches RefIterate(const std::string& src, const std::regex& re) {
    Matches out;
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re); it != std::sregex_iterator();
         it++) {
        out.emplace_back((std::size_t)it->position(), it->str());
    }
    return out;
}

Matches RefStageIo(const std::string& src) {
    return RefIterate(src,
                      std::regex(R"((?:^|\s)(in|out)\s[\s\w]+\s(\w+)\s*(?:\[\])?\s*;)",
                                 std::regex::ECMAScript | std::regex::multiline));
}

Matches RefTextureSlots(const std::string& src) {
    return RefIterate(
        src, std::regex(R"(uniform\s+sampler2D\s+g_Texture(\d+))", std::regex::ECMAScript));
}

// Positions are relative to the string handed to regex_search, so recover the
// absolute offset from the iterator the same way for both implementations.
Matches NewStageIo(const std::string& src) {
    Matches out;
    ForEachStageIoDecl(src, [&](const std::smatch& m) {
        out.emplace_back((std::size_t)(m[0].first - src.begin()), m.str(0));
    });
    return out;
}

Matches NewTextureSlots(const std::string& src) {
    Matches out;
    ForEachTextureSlotUniform(src, [&](const std::smatch& m) {
        out.emplace_back((std::size_t)(m[0].first - src.begin()), m.str(0));
    });
    return out;
}

void CheckAllScans(const std::string& src) {
    CAPTURE(src);
    for (std::size_t pos = 0; pos <= src.size() + 1; pos++) {
        CAPTURE(pos);
        CHECK(FindIfOrEndif(src, pos) == RefIfOrEndif(src, pos));
    }
    CHECK(FindLastDeclarationLine(src) == RefLastDeclarationLine(src));
    CHECK(NewStageIo(src) == RefStageIo(src));
    CHECK(NewTextureSlots(src) == RefTextureSlots(src));
}

const std::vector<std::string> kHandWritten = {
    "",
    "\n",
    "in vec2 a;",
    "out vec4 b;\nin vec2 c;\n",
    "smooth out vec4 v_Color;\nflat in int v_Id[];\n",
    "in vec4 v_Color[] ;\n",
    "inout vec2 x;\n void main(in vec2 p;",
    "\tin\tvec2\ta ;\n  out highp vec3 b;",
    "vec2 main(){}in vec2 notAfterSpace;",
    "  in vec2 a; in vec2 b;in vec2 c;",
    "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture12;\n",
    "uniform  sampler2D\tg_Texture3; uniformsampler2D g_Texture4;",
    "uniform uniform sampler2D g_Texture7;",
    "uniform sampler2D g_TextureX; uniform sampler2D g_Texture99999999999;",
    "#if A\n#endif\n#ifdef B\n#elif C\n#endif",
    "#endif#if",
    "attribute vec3 a_Position;\nvarying vec2 v;\nuniform mat4 m;\nvoid main(){}",
    "\nattribute ",
    "x\nuniform ",
    "\nuniform x\nvarying y\nattribute z\n",
    "\nuniformx\n varying y\n",
};
} // namespace

TEST_SUITE("ShaderSourceScan") {
    TEST_CASE("hand-written edge cases match the regex walks") {
        for (const auto& src : kHandWritten) CheckAllScans(src);
    }

    // Random sources assembled from the tokens these patterns care about, so
    // keywords land at line starts, after tabs, glued to identifiers, etc.
    TEST_CASE("random shader-like sources match the regex walks") {
        const std::vector<std::string> tokens = {
            "in", "out",       "inout",   " ",   "\t",      "\n",        "\r\n",      ";",
            "[]", "vec4",      "a1",      "_x",  "uniform", "sampler2D", "g_Texture", "0",
            "12", "attribute", "varying", "#if", "#ifdef",  "#endif",    "#else",     "void main(",
            "{",  "}",         "(",       ")",   "flat",    "highp",
        };
        std::mt19937                               rng(12345);
        std::uniform_int_distribution<std::size_t> pick(0, tokens.size() - 1);
        std::uniform_int_distribution<int>         len(0, 40);
        for (int i = 0; i < 3000; i++) {
            std::string src;
            for (int n = len(rng); n > 0; n--) src += tokens[pick(rng)];
            CheckAllScans(src);
        }
    }
}
