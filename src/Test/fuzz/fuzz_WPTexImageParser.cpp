// libFuzzer entry point for WPTexImageParser::Parse.
//
// .tex is a binary, multi-section, LZ4-compressed format with version-branched
// parse paths (TEXV, TEXI, TEXB sections). The parser opens a path through a
// VFS, so the harness wraps the fuzzer input in a one-shot in-memory Fs.
//
// Build / run: see fuzz_WPMdlParser.cpp header comments — same flags.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Fs/MemBinaryStream.h"
#include "Fs/VFS.h"
#include "WPTexImageParser.hpp"

namespace
{

class MockFs : public wallpaper::fs::Fs {
public:
    void AddFile(std::string path, std::vector<uint8_t> data) {
        m_files[std::move(path)] = std::move(data);
    }
    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) > 0;
    }
    std::shared_ptr<wallpaper::fs::IBinaryStream> Open(std::string_view path) override {
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        auto copy = it->second;
        return std::make_shared<wallpaper::fs::MemBinaryStream>(std::move(copy));
    }
    std::shared_ptr<wallpaper::fs::IBinaryStreamW> OpenW(std::string_view) override {
        return nullptr;
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Cap input — .tex headers can declare large allocations; ASAN trips on
    // truly absurd sizes anyway, but bound the fuzzer's working set.
    if (size > 16u * 1024u * 1024u) return 0;

    wallpaper::fs::VFS vfs;
    auto               mockFs = std::make_unique<MockFs>();
    mockFs->AddFile("/materials/fuzz.tex",
                    std::vector<uint8_t>(data, data + size));
    vfs.Mount("/assets", std::move(mockFs));

    wallpaper::WPTexImageParser parser(&vfs);
    (void)parser.Parse("fuzz");
    return 0;
}
