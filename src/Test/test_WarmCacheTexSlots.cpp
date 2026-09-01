#include <doctest.h>

#include "WPShaderParser.hpp" // transitively pulls ShaderCode/ShaderType/WPShaderInfo
#include "Fs/VFS.h"
#include "Fs/PhysicalFs.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
        // The cold path compiles and writes SPV to /cache before returning.  If
        // the shader fails to compile nothing is written, and the next REQUIRE
        // fails LOUDLY (we do not silently fall through to a miss).
        REQUIRE(units.size() == 1);
        // Preprocessor ran on the cold path too.
        CHECK(units[0].preprocess_info.active_tex_slots.count(0u) == 1);
        WPShaderParser::ClearSpvMemo(); // end-of-load, as finalizeParse does
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

// ---------------------------------------------------------------------------
// Shader-compile lifetime contract.
//
// A scene load owns the `SceneShader` objects it compiles into, and that
// ownership ends when the load ends — a throw out of WPSceneParser::Parse, or
// an effect chain dropped after some of its passes already compiled.  Nothing
// inside the shader compiler may still be pointing at those buffers by then,
// because the NEXT load runs on the same thread and would write straight
// through a stale pointer into freed heap.
// ---------------------------------------------------------------------------

namespace
{
// A distinct fragment source per tag.  The cache key is the sha1 of the
// post-preprocess unit source, so varying the constant guarantees a distinct
// cache path -> a guaranteed MISS -> the cold-compile branch under test.
std::vector<WPShaderUnit> MakeTaggedUnits(int tag) {
    std::string src = "void main() {\n"
                      "    gl_FragColor = vec4(" +
                      std::to_string(tag) +
                      ".0 / 255.0, 0.0, 0.0, 1.0);\n"
                      "}\n";
    std::vector<WPShaderUnit> units;
    units.push_back(WPShaderUnit { ShaderType::FRAGMENT, std::move(src), {} });
    return units;
}

// Per-case scratch cache dir + VFS mount.  PID-suffixed so parallel ctest
// shards / mutation workers don't collide on one path.
struct ScratchCache {
    std::string dir;

    explicit ScratchCache(std::string_view tag)
        : dir("/tmp/wek_" + std::string(tag) + "_" + std::to_string(::getpid())) {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }
    ~ScratchCache() { std::filesystem::remove_all(dir); }
    ScratchCache(const ScratchCache&)            = delete;
    ScratchCache& operator=(const ScratchCache&) = delete;

    void mount(fs::VFS& vfs) const {
        REQUIRE(vfs.Mount("/cache", fs::CreatePhysicalFs(dir, true), "cache"));
        REQUIRE(vfs.IsMounted("cache"));
    }
};

// One scene load's shader work for `shader`: compile, then run the end-of-load
// step WPSceneParser::finalizeParse runs.
bool CompileOneLoad(fs::VFS& vfs, const std::shared_ptr<SceneShader>& shader, int tag) {
    auto                         units = MakeTaggedUnits(tag);
    WPShaderInfo                 info;
    std::vector<WPShaderTexInfo> texs;
    bool ok = WPShaderParser::CompileToSpv("shader_pub", units, shader->codes, vfs, &info, texs);
    WPShaderParser::ClearSpvMemo();
    return ok;
}
} // namespace

TEST_CASE("an abandoned shader compile is not written through by a later load") {
    EnsureGlslang();
    ScratchCache cache("shader_abandoned");
    fs::VFS      vfs;
    cache.mount(vfs);

    // Load 1 compiles, then dies before its end-of-load step — the shape of a
    // throw unwinding out of WPSceneParser::Parse.
    auto abandoned = std::make_shared<SceneShader>();
    {
        auto                         units = MakeTaggedUnits(11);
        WPShaderInfo                 info;
        std::vector<WPShaderTexInfo> texs;
        REQUIRE(
            WPShaderParser::CompileToSpv("shader_pub", units, abandoned->codes, vfs, &info, texs));
    }
    // Whatever load 1 legitimately produced is the ONLY thing this shader may
    // ever hold: the load that owned it is over.
    const std::vector<ShaderCode> after_load1 = abandoned->codes;

    // Load 2 runs to completion with an unrelated shader.
    auto second = std::make_shared<SceneShader>();
    REQUIRE(CompileOneLoad(vfs, second, 22));

    CHECK(abandoned->codes == after_load1);
    CHECK_FALSE(second->codes.empty());
}

TEST_CASE("a compile whose shader died before the next load is not written through") {
    EnsureGlslang();
    ScratchCache cache("shader_dead_owner");
    fs::VFS      vfs;
    cache.mount(vfs);

    std::weak_ptr<SceneShader> dead;
    {
        // NOT make_shared: make_shared co-allocates the control block with the
        // object, so the weak_ptr below would keep the whole chunk mapped and a
        // write into the destroyed shader would be invisible to ASAN.  Keep the
        // separate allocation — "tidying" this to make_shared disarms the test
        // with no visible signal.
        std::shared_ptr<SceneShader> doomed(new SceneShader());
        dead = doomed;

        auto                         units = MakeTaggedUnits(33);
        WPShaderInfo                 info;
        std::vector<WPShaderTexInfo> texs;
        REQUIRE(
            WPShaderParser::CompileToSpv("shader_pub", units, doomed->codes, vfs, &info, texs));
    }
    REQUIRE(dead.expired());

    // The next load must not touch the freed shader.
    auto live = std::make_shared<SceneShader>();
    REQUIRE(CompileOneLoad(vfs, live, 44));
    CHECK_FALSE(live->codes.empty());
}

TEST_CASE("two materials sharing one shader source both receive SPV from one compile") {
    EnsureGlslang();
    ScratchCache cache("shader_dedupe");
    fs::VFS      vfs;
    cache.mount(vfs);

    // Both compiles happen inside ONE load, before any end-of-load step, so
    // they exercise the in-memory dedupe rather than the disk cache.
    auto                         a       = std::make_shared<SceneShader>();
    auto                         b       = std::make_shared<SceneShader>();
    auto                         units_a = MakeTaggedUnits(55);
    auto                         units_b = MakeTaggedUnits(55);
    WPShaderInfo                 info_a, info_b;
    std::vector<WPShaderTexInfo> texs;
    REQUIRE(WPShaderParser::CompileToSpv("shader_pub", units_a, a->codes, vfs, &info_a, texs));
    REQUIRE(WPShaderParser::CompileToSpv("shader_pub", units_b, b->codes, vfs, &info_b, texs));
    WPShaderParser::ClearSpvMemo();

    CHECK_FALSE(a->codes.empty());
    CHECK_FALSE(b->codes.empty());
    CHECK(a->codes == b->codes);
    // One disk entry for the shared sha1 — the compile ran once.
    CHECK(CountFilesRec(cache.dir) == 1);
}

// ---------------------------------------------------------------------------
// Hostile input on the same two paths.
//
// Combo values and the on-disk SPV cache are both attacker-adjacent: combos
// come straight out of workshop scene/shader files, and a cache entry can be
// half-written by a crash or a full disk.  What they feed is glslang and
// vkCreateShaderModule.
// ---------------------------------------------------------------------------

TEST_CASE("a hostile TRAILSUBDIVISION does not overflow the geometry max_vertices") {
    EnsureGlslang();

    fs::VFS                      vfs; // no cache mount — synchronous compile
    std::vector<WPShaderTexInfo> texs;
    std::vector<ShaderCode>      codes;

    SUBCASE("value past INT_MAX/2 clamps instead of wrapping negative") {
        WPShaderInfo info;
        info.combos["TRAILSUBDIVISION"] = "2000000000";
        std::vector<WPShaderUnit> units;
        units.push_back(WPShaderUnit { ShaderType::GEOMETRY, "void main() {}\n", {} });
        CHECK_NOTHROW(
            (void)WPShaderParser::CompileToSpv("hostile", units, codes, vfs, &info, texs));
        CHECK(units[0].src.find("max_vertices = 256") != std::string::npos);
    }

    SUBCASE("non-numeric value falls back to the default instead of throwing") {
        WPShaderInfo info;
        info.combos["TRAILSUBDIVISION"] = "not-a-number";
        std::vector<WPShaderUnit> units;
        units.push_back(WPShaderUnit { ShaderType::GEOMETRY, "void main() {}\n", {} });
        CHECK_NOTHROW(
            (void)WPShaderParser::CompileToSpv("hostile", units, codes, vfs, &info, texs));
        CHECK(units[0].src.find("max_vertices = 4") != std::string::npos);
    }
}

TEST_CASE("a truncated SPV cache entry is rejected rather than handed to the driver") {
    EnsureGlslang();
    ScratchCache cache("shader_truncated");
    fs::VFS      vfs;
    cache.mount(vfs);

    std::vector<WPShaderTexInfo> texs;
    {
        auto                    units = MakeTaggedUnits(66);
        WPShaderInfo            info;
        std::vector<ShaderCode> codes;
        REQUIRE(WPShaderParser::CompileToSpv("shader_pub", units, codes, vfs, &info, texs));
        REQUIRE_FALSE(codes.empty());
    }
    WPShaderParser::ClearSpvMemo();

    // Corrupt the entry the compile just wrote: keep a well-formed header, then
    // declare a payload far larger than what is actually left in the file.
    std::string entry;
    for (auto& e : std::filesystem::recursive_directory_iterator(cache.dir))
        if (e.is_regular_file()) entry = e.path().string();
    REQUIRE_FALSE(entry.empty());
    {
        std::ofstream out(entry, std::ios::binary | std::ios::trunc);
        out.write("SPVS0001", 8);
        out.put('\0'); // WriteVersion emits a 9-byte record
        const uint32_t count = 1;
        const uint32_t size  = 4096;
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
        out.write("\0\0\0\0", 4); // 4 of the 4096 declared bytes
    }

    // Same source -> same sha1 -> the corrupt file is a cache HIT.
    auto                    units = MakeTaggedUnits(66);
    WPShaderInfo            info;
    std::vector<ShaderCode> codes;
    CHECK_FALSE(WPShaderParser::CompileToSpv("shader_pub", units, codes, vfs, &info, texs));
    CHECK(codes.empty());
}
