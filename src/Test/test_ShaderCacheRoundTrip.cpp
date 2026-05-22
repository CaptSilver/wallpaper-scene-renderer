#include <doctest.h>

#include "WPShaderParser.hpp"
#include "Fs/VFS.h"
#include "Fs/PhysicalFs.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace wallpaper;

// Behavioral proof of the raw-keyed warm cache: compile a shader through the
// real glslang path twice against the SAME on-disk cache dir.  The first call
// misses (Preprocessor + compile, written to disk on flush); the second call
// must hit the warm cache and return the byte-identical SPV WITHOUT running
// Preprocessor at all (PreprocessRunCount stays 0 across the second call).
namespace
{
// glslang keyword tables are process-global; init once and never finalize
// (tearing them down mid-run is the documented thread-safety hazard).
void initGlslangOnce() {
    static const bool once = [] {
        WPShaderParser::InitGlslang();
        return true;
    }();
    (void)once;
}

std::filesystem::path makeCacheDir() {
    auto base = std::filesystem::temp_directory_path() /
                ("wek_shadercache_rt_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);
    return base;
}

// Trivial-but-valid GLSL the host glslang toolchain compiles headlessly.
std::vector<WPShaderUnit> trivialUnits() {
    std::vector<WPShaderUnit> units;
    units.push_back(
        WPShaderUnit { ShaderType::FRAGMENT, "void main() { gl_FragColor = vec4(1.0); }", {} });
    return units;
}
} // namespace

TEST_SUITE("ShaderCacheRoundTrip") {
    TEST_CASE("warm second compile hits the cache and skips Preprocessor") {
        initGlslangOnce();

        auto    cache_dir = makeCacheDir();
        fs::VFS vfs;
        REQUIRE(vfs.Mount(
            "/cache", fs::CreatePhysicalFs(cache_dir.string(), /*create=*/true), "cache"));
        REQUIRE(vfs.IsMounted("cache"));

        WPShaderInfo                 info; // empty combos
        std::vector<WPShaderTexInfo> texs;

        // ---- Cold compile: miss -> defer -> flush writes SPV to disk ----
        WPShaderParser::ResetPreprocessRunCount();
        std::vector<ShaderCode> cold_codes;
        {
            auto units = trivialUnits();
            REQUIRE(WPShaderParser::CompileToSpv("rt", units, cold_codes, vfs, &info, texs));
        }
        // Miss path defers: codes not populated until the flush.
        CHECK(cold_codes.empty());
        int cold_preprocess = WPShaderParser::PreprocessRunCount();
        CHECK(cold_preprocess > 0); // Preprocessor ran on the miss

        WPShaderParser::FlushPendingCompilations(vfs);
        REQUIRE_FALSE(cold_codes.empty()); // compiled SPV now in hand

        // ---- Warm compile: hit -> synchronous load, no Preprocessor ----
        WPShaderParser::ResetPreprocessRunCount();
        std::vector<ShaderCode> warm_codes;
        {
            auto units = trivialUnits();
            REQUIRE(WPShaderParser::CompileToSpv("rt", units, warm_codes, vfs, &info, texs));
        }
        // Hit path returns the SPV synchronously (no flush needed) and must not
        // have touched Preprocessor.
        CHECK(WPShaderParser::PreprocessRunCount() == 0);
        REQUIRE_FALSE(warm_codes.empty());

        // ---- SPV is byte-identical between the cold and warm paths ----
        // ShaderCode is the SPIR-V word vector itself (std::vector<unsigned int>).
        REQUIRE(cold_codes.size() == warm_codes.size());
        for (size_t i = 0; i < cold_codes.size(); ++i) {
            CHECK(cold_codes[i] == warm_codes[i]);
        }

        // No deferred work should remain after a pure hit.
        WPShaderParser::FlushPendingCompilations(vfs);

        std::error_code ec;
        std::filesystem::remove_all(cache_dir, ec); // remove only this test's scratch dir
    }
} // TEST_SUITE ShaderCacheRoundTrip
