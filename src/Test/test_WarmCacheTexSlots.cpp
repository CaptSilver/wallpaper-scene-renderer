#include <doctest.h>

#include "WPShaderParser.hpp" // transitively pulls ShaderCode/ShaderType/WPShaderInfo
#include "Fs/VFS.h"
#include "Fs/PhysicalFs.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h> // getpid

using namespace wallpaper;

namespace
{
// One-time glslang init for this binary.  CompileToSpv -> CompileShaderUnits
// needs the process-global glslang tables.  Guard so repeat runs in one process
// don't re-init; never FinalGlslang() (the process exits right after, and a
// paired Final after other suites' compiles would be order-fragile).
void EnsureGlslang() {
    static std::once_flag once;
    std::call_once(once, [] { WPShaderParser::InitGlslang(); });
}

// A fragment unit that DECLARES g_Texture0 so the preprocessor records slot 0
// in active_tex_slots (the set WPSceneParser uses to decide which material
// texture slots to KEEP vs clear()).  The slot is recorded from the `uniform
// sampler2D g_TextureN` DECLARATION (regex), not from a sample call — so the
// body stays a trivial constant write.  A raw texture2D() call here would trip
// the WE fixup chain's combined-sampler construct path ("cannot construct
// sampler/image") and fail glslang; we only need a compilable unit that carries
// the declaration so the cold run can populate the SPV cache.
std::string TexFragSrc() {
    return "uniform sampler2D g_Texture0;\n"
           "void main() {\n"
           "    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
           "}\n";
}

std::vector<WPShaderUnit> MakeUnits() {
    std::vector<WPShaderUnit> units;
    units.push_back(WPShaderUnit { ShaderType::FRAGMENT, TexFragSrc(), {} });
    return units;
}

// Count regular files anywhere under `dir`.  Used to prove the cold run actually
// compiled + wrote a cache entry (so the warm run truly hits the disk cache).
size_t CountFilesRec(const std::string& dir) {
    size_t n = 0;
    if (! std::filesystem::exists(dir)) return 0;
    for (auto& e : std::filesystem::recursive_directory_iterator(dir))
        if (e.is_regular_file()) ++n;
    return n;
}
} // namespace

// LD2-B regression class: a warm SPV-cache HIT must STILL run the preprocessor
// that populates active_tex_slots.  WPSceneParser clears every material texture
// slot NOT in that set; if a cache hit skips the preprocessor the set is empty
// -> all textures stripped on the 2nd/warm run (valid SPV, zero failed passes,
// invisible to every other unit test).
TEST_CASE("warm SPV cache hit still populates active_tex_slots") {
    EnsureGlslang();

    // Unique scratch cache dir, self-cleaned (project test convention uses a
    // named /tmp dir; PID-suffix it so parallel ctest shards don't collide).
    const std::string cache_dir =
        "/tmp/wek_warm_cache_texslots_" + std::to_string(::getpid());
    std::filesystem::remove_all(cache_dir);
    std::filesystem::create_directories(cache_dir);

    auto mount_cache = [&](fs::VFS& vfs) {
        REQUIRE(vfs.Mount("/cache", fs::CreatePhysicalFs(cache_dir, true), "cache"));
        REQUIRE(vfs.IsMounted("cache"));
    };

    // ---- Run 1: COLD (empty cache) -> miss -> deferred compile, flush to disk.
    {
        fs::VFS                      vfs;
        mount_cache(vfs);
        WPShaderInfo                 info;
        auto                         units = MakeUnits();
        std::vector<ShaderCode>      codes;
        std::vector<WPShaderTexInfo> texs;
        REQUIRE(WPShaderParser::CompileToSpv("oracle_pin", units, codes, vfs, &info, texs));
        // Cold path defers the glslang compile; flush runs it and writes SPV to
        // /cache.  If the shader fails to compile, nothing is written and the
        // next REQUIRE fails LOUDLY (we do not silently fall through to a miss).
        WPShaderParser::FlushPendingCompilations(vfs);
        REQUIRE(units.size() == 1);
        // Preprocessor ran on the cold path too.
        CHECK(units[0].preprocess_info.active_tex_slots.count(0u) == 1);
    }

    // The warm run can only HIT the cache if the cold run actually wrote SPV.
    REQUIRE(CountFilesRec(cache_dir) > 0);

    // ---- Run 2: WARM (cache populated) -> HIT -> must STILL preprocess first.
    {
        fs::VFS                      vfs;
        mount_cache(vfs);
        WPShaderInfo                 info;
        auto                         units = MakeUnits(); // fresh: empty active_tex_slots
        std::vector<ShaderCode>      codes;
        std::vector<WPShaderTexInfo> texs;
        REQUIRE(units[0].preprocess_info.active_tex_slots.empty()); // precondition
        REQUIRE(WPShaderParser::CompileToSpv("oracle_pin", units, codes, vfs, &info, texs));
        // The warm hit returns early after loading SPV from disk — but the
        // preprocessor must have run first, so slot 0 is still recorded.  THIS
        // is the assertion that the LD2-B re-ordering would break.
        CHECK_FALSE(units[0].preprocess_info.active_tex_slots.empty());
        CHECK(units[0].preprocess_info.active_tex_slots.count(0u) == 1);
        // The warm path produced usable SPV (cache load succeeded).
        CHECK_FALSE(codes.empty());
    }

    std::filesystem::remove_all(cache_dir);
}
