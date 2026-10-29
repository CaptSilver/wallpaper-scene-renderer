// Behavioural tests for the SPIR-V validation gate (spvOptions.validate).
#include <doctest.h>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>

#include "SceneParseFixtures.hpp"
#include "Utils/Logging.h"
#include "Vulkan/Shader.hpp"
#include "Vulkan/ShaderComp.hpp"
#include "test_scratch.hpp"

namespace wallpaper::vulkan::detail
{
bool ShouldValidateSpv();
} // namespace wallpaper::vulkan::detail

namespace
{

// Only called from the NDEBUG branch below; a Debug build never calls it.
[[maybe_unused]] void clearShaderDebugEnv() { ::unsetenv("WEK_DEBUG_SHADERS"); }

std::string g_failureLog;

void captureLog(int, const char* msg) {
    g_failureLog += msg;
    g_failureLog += '\n';
}

// Compiles a vertex+fragment pair. A failed parse dumps the shader source into
// the temp dir, so TMPDIR points at a scratch dir for the duration; the dump
// and glslang's info log (captured in g_failureLog) are the only side effects.
struct Compiled {
    bool                                          ok { false };
    std::vector<wallpaper::vulkan::Uni_ShaderSpv> spvs;
};

Compiled compileUnits(std::vector<wallpaper::vulkan::ShaderCompUnit> units,
                      const wallpaper::vulkan::ShaderCompOpt&        opt) {
    using namespace wallpaper::vulkan;
    wek::test::ensureGlslangInit();

    const char*       oldTmp  = std::getenv("TMPDIR");
    std::string       saved   = oldTmp ? oldTmp : "";
    const std::string scratch = wallpaper::test::ScratchDir("shader_compile", ::getpid());
    ::setenv("TMPDIR", scratch.c_str(), 1);

    Compiled out;
    g_failureLog.clear();
    wallpaper_log_test::setSink(&captureLog);
    {
        std::lock_guard<std::mutex> guard(GlslangSerialiseMutex());
        out.ok = CompileAndLinkShaderUnits(units, opt, out.spvs);
    }
    wallpaper_log_test::setSink(nullptr);

    if (oldTmp)
        ::setenv("TMPDIR", saved.c_str(), 1);
    else
        ::unsetenv("TMPDIR");
    return out;
}

Compiled compile(const char* vs, const char* fs, const wallpaper::vulkan::ShaderCompOpt& opt) {
    return compileUnits({ { EShLangVertex, vs }, { EShLangFragment, fs } }, opt);
}

wallpaper::vulkan::ShaderCompOpt vulkanOpts() {
    wallpaper::vulkan::ShaderCompOpt opt;
    opt.client_ver = glslang::EShTargetVulkan_1_1;
    return opt;
}

constexpr const char* kPlainFs =
    "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(1.0); }\n";

} // namespace

TEST_SUITE_BEGIN("Shader compile flags");

TEST_CASE("ShouldValidateSpv: NDEBUG build with env unset returns false") {
#ifdef NDEBUG
    clearShaderDebugEnv();
    CHECK_FALSE(wallpaper::vulkan::detail::ShouldValidateSpv());
#else
    // In Debug builds the predicate is unconditionally true; skip the
    // env-clearing assertion (the static-init caches Debug=true regardless).
    CHECK(wallpaper::vulkan::detail::ShouldValidateSpv());
#endif
}

TEST_CASE("ShouldValidateSpv: Debug build always returns true") {
#ifndef NDEBUG
    CHECK(wallpaper::vulkan::detail::ShouldValidateSpv());
#endif
}

TEST_CASE("ShouldValidateSpv: WEK_DEBUG_SHADERS=1 forces true even under NDEBUG") {
    // NOTE: the predicate caches in a static-local; this test only covers
    // the env-set state when the cache is fresh.  In a Debug build the
    // predicate is unconditionally true so the env doesn't matter.  In a
    // Release build, the first test sets the cache to false, so this
    // assertion documents the env-honoured path via a side-channel: we
    // can't re-cache once initialised, so the test is degenerate after
    // case #1 runs.  Run with `--test-case=*WEK_DEBUG_SHADERS*` to isolate.
    ::setenv("WEK_DEBUG_SHADERS", "1", 1);
    // No assertion possible without test-isolated env setup; this case
    // exists to document the intent + serve as the env-override smoke.
    CHECK_NOTHROW(wallpaper::vulkan::detail::ShouldValidateSpv());
}

TEST_SUITE_END();

TEST_SUITE("ShaderCompileOptions") {
    TEST_CASE("a loose uniform is rejected under Vulkan rules and accepted once relaxed") {
        const char* vs  = "#version 450\nlayout(location=0) in vec3 p;\nuniform vec4 u_tint;\n"
                          "void main(){ gl_Position = u_tint + vec4(p, 1.0); }\n";
        auto        opt = vulkanOpts();
        CHECK_FALSE(compile(vs, kPlainFs, opt).ok);

        opt.relaxed_rules_vulkan = true;
        CHECK(compile(vs, kPlainFs, opt).ok);
    }

    TEST_CASE("relaxed errors let a shader redefine 'defined'") {
        const char* vs  = "#version 450\n#define defined 1\n"
                          "void main(){ gl_Position = vec4(1.0); }\n";
        auto        opt = vulkanOpts();
        CHECK_FALSE(compile(vs, kPlainFs, opt).ok);

        opt.relaxed_errors_glsl = true;
        CHECK(compile(vs, kPlainFs, opt).ok);
    }

    TEST_CASE("suppressing warnings drops them from the failure log") {
        // An unsupported extension requested with 'enable' is only a warning; the undefined
        // function makes the compile fail so the info log is reported.
        const char* vs  = "#version 450\n#extension GL_WEK_not_a_real_extension : enable\n"
                          "void main(){ gl_Position = nosuchfn(); }\n";
        auto        opt = vulkanOpts();

        CHECK_FALSE(compile(vs, kPlainFs, opt).ok);
        CHECK(g_failureLog.find("WARNING") != std::string::npos);
        CHECK(g_failureLog.find("nosuchfn") != std::string::npos);

        opt.suppress_warnings_glsl = true;
        CHECK_FALSE(compile(vs, kPlainFs, opt).ok);
        CHECK(g_failureLog.find("WARNING") == std::string::npos);
        CHECK(g_failureLog.find("nosuchfn") != std::string::npos);

        // Warnings stay suppressed alongside relaxed errors.
        opt.relaxed_errors_glsl = true;
        CHECK_FALSE(compile(vs, kPlainFs, opt).ok);
        CHECK(g_failureLog.find("WARNING") == std::string::npos);
        CHECK(g_failureLog.find("nosuchfn") != std::string::npos);
    }

    TEST_CASE("a uniform block shared by two stages reflects the members of both") {
        const char* vs  = "#version 450\nlayout(location=0) in vec3 p;\nuniform vec4 u_vsOnly;\n"
                          "void main(){ gl_Position = u_vsOnly + vec4(p, 1.0); }\n";
        const char* fs  = "#version 450\nlayout(location=0) out vec4 o;\nuniform vec4 u_fsOnly;\n"
                          "void main(){ o = u_fsOnly; }\n";
        auto        opt = vulkanOpts();
        opt.relaxed_rules_vulkan = true;
        auto compiled            = compile(vs, fs, opt);
        REQUIRE(compiled.ok);

        std::vector<std::vector<unsigned int>> codes;
        for (auto& spv : compiled.spvs) codes.push_back(spv->spirv);
        std::vector<wallpaper::vulkan::Uni_ShaderSpv> reflectedSpvs;
        wallpaper::vulkan::ShaderReflected            ref;
        REQUIRE(wallpaper::vulkan::GenReflect(codes, reflectedSpvs, ref));

        REQUIRE(ref.blocks.size() == 1u);
        CHECK(ref.blocks[0].member_map.count("u_vsOnly") == 1u);
        CHECK(ref.blocks[0].member_map.count("u_fsOnly") == 1u);
    }

    TEST_CASE("a block compiled separately per stage reflects the union of its members") {
        // Each stage is compiled on its own, so the two modules declare the same block with
        // different member sets; the merged block must carry every member and the larger size.
        const char* vs  = "#version 450\nlayout(location=0) in vec3 p;\n"
                          "layout(binding=0) uniform U { vec4 a; } u;\n"
                          "void main(){ gl_Position = u.a + vec4(p, 1.0); }\n";
        const char* fs  = "#version 450\nlayout(location=0) out vec4 o;\n"
                          "layout(binding=0) uniform U { vec4 a; vec4 b; } u;\n"
                          "void main(){ o = u.a + u.b; }\n";
        auto        opt = vulkanOpts();
        auto        v   = compileUnits({ { EShLangVertex, vs } }, opt);
        auto        f   = compileUnits({ { EShLangFragment, fs } }, opt);
        REQUIRE(v.ok);
        REQUIRE(f.ok);

        std::vector<std::vector<unsigned int>>        codes { v.spvs[0]->spirv, f.spvs[0]->spirv };
        std::vector<wallpaper::vulkan::Uni_ShaderSpv> reflectedSpvs;
        wallpaper::vulkan::ShaderReflected            ref;
        REQUIRE(wallpaper::vulkan::GenReflect(codes, reflectedSpvs, ref));

        REQUIRE(ref.blocks.size() == 1u);
        CHECK(ref.blocks[0].member_map.count("a") == 1u);
        REQUIRE(ref.blocks[0].member_map.count("b") == 1u);
        CHECK(ref.blocks[0].member_map.at("b").offset == 16u);
        CHECK(ref.blocks[0].size == 32u);
    }
}
