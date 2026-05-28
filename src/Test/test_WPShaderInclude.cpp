#include <doctest.h>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "WPShaderParser.hpp"
#include "Fs/VFS.h"
#include "Fs/Fs.h"
#include "Fs/MemBinaryStream.h"

using wallpaper::WPShaderInfo;
using wallpaper::WPShaderParser;
using wallpaper::fs::Fs;
using wallpaper::fs::IBinaryStream;
using wallpaper::fs::IBinaryStreamW;
using wallpaper::fs::MemBinaryStream;
using wallpaper::fs::VFS;

namespace
{
// Minimal in-memory Fs holding text files. Mounted at /assets; VFS strips the
// mount prefix (GetPathInMount), so a lookup of "/assets/shaders/common.h"
// reaches this Fs as "/shaders/common.h" — add files under that stripped key.
class MemFs : public Fs {
public:
    void add(std::string path, std::string data) {
        std::vector<uint8_t> bytes(data.begin(), data.end());
        m_files[std::move(path)] = std::move(bytes);
    }
    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) > 0;
    }
    std::shared_ptr<IBinaryStream> Open(std::string_view path) override {
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        auto copy = it->second;
        return std::make_shared<MemBinaryStream>(std::move(copy));
    }
    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view) override { return nullptr; }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

std::unique_ptr<VFS> makeAssetsVfs(std::unique_ptr<MemFs> memfs) {
    auto vfs = std::make_unique<VFS>();
    REQUIRE(vfs->Mount("/assets", std::move(memfs)));
    return vfs;
}
} // namespace

TEST_SUITE("PreShaderSrc #include handling") {
    TEST_CASE("tolerates #include with no quotes (no crash, body survives)") {
        auto         memfs = std::make_unique<MemFs>();
        auto         vfs   = makeAssetsVfs(std::move(memfs));
        WPShaderInfo info;
        // Malformed: a top-level #include with no quoted name. PreShaderSrc
        // collects it into the include block; LoadGlslInclude re-scans and
        // (before the fix) substr(0, npos) returned the whole line as a bogus
        // include name, emitting a "//-----include <garbage>" banner.
        std::string src = "#include\nvoid main(){}\n";
        std::string out;
        CHECK_NOTHROW(out = WPShaderParser::PreShaderSrc(*vfs, src, &info, {}));
        // The shader body survived.
        CHECK(out.find("main") != std::string::npos);
        // Pre-fix bug: the whole malformed line became the include name → a
        // "//-----include " banner was emitted. After the fix the line is passed
        // through verbatim with no banner.
        CHECK(out.find("//-----include ") == std::string::npos);
    }

    TEST_CASE("well-formed #include inlines the file content (positive control)") {
        auto memfs = std::make_unique<MemFs>();
        // PreShaderSrc resolves includes under /assets/shaders/; VFS strips the
        // /assets mount prefix, so the key is /shaders/common.h.
        memfs->add("/shaders/common.h", "float WEK_INCLUDE_MARKER = 1.0;\n");
        auto         vfs = makeAssetsVfs(std::move(memfs));
        WPShaderInfo info;
        std::string  src = "#include \"common.h\"\nvoid main(){}\n";
        std::string  out;
        CHECK_NOTHROW(out = WPShaderParser::PreShaderSrc(*vfs, src, &info, {}));
        // The include's content was inlined.
        CHECK(out.find("WEK_INCLUDE_MARKER") != std::string::npos);
        CHECK(out.find("main") != std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Fuzz crash regression replay.
//
// Iterates tests/fixtures/fuzz_regressions/WPShaderParser/*.bin and feeds
// each file through the same entry point fuzz_WPShaderParser drives.
// ---------------------------------------------------------------------------

#include "test_data_root.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

#include "WPShaderParser.hpp"

TEST_SUITE("regression: minimised fuzz crashes") {
    TEST_CASE("regression: minimised fuzz crashes round-trip cleanly") {
        namespace fs2 = std::filesystem;
        const fs2::path dir = wallpaper::test::test_data_root()
                              / "fuzz_regressions" / "WPShaderParser";
        if (! fs2::exists(dir)) return;
        for (auto& entry : fs2::directory_iterator(dir)) {
            if (entry.path().extension() != ".bin") continue;
            SUBCASE(entry.path().filename().string().c_str()) {
                std::ifstream in(entry.path(), std::ios::binary);
                std::string src(std::istreambuf_iterator<char>(in), {});
                VFS                                vfs;
                WPShaderInfo                       info;
                std::vector<wallpaper::WPShaderTexInfo> texs;
                CHECK_NOTHROW(
                    (void)WPShaderParser::PreShaderSrc(vfs, src, &info, texs));
            }
        }
    }
}
