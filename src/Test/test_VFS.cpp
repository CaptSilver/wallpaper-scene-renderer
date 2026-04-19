#include <doctest.h>

#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;

namespace
{

// Minimal in-memory Fs for testing VFS routing.
class MemFs : public Fs {
public:
    void add(std::string path, std::vector<uint8_t> data) {
        m_files[std::move(path)] = std::move(data);
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
    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view) override {
        return nullptr;
    }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

} // namespace

TEST_SUITE("VFS") {

TEST_CASE("Mount with trailing slash in mountPoint fails CheckMountPoint") {
    VFS  v;
    auto fs = std::make_unique<MemFs>();
    CHECK_FALSE(v.Mount("/assets/", std::move(fs)));
}

TEST_CASE("Mount null Fs returns false") {
    VFS v;
    CHECK_FALSE(v.Mount("/x", std::unique_ptr<Fs>(nullptr)));
}

TEST_CASE("Mount + IsMounted named entry") {
    VFS  v;
    auto fs = std::make_unique<MemFs>();
    REQUIRE(v.Mount("/assets", std::move(fs), "pkg"));
    CHECK(v.IsMounted("pkg"));
    CHECK_FALSE(v.IsMounted("missing"));
}

TEST_CASE("Contains matches only paths under a mount point") {
    VFS  v;
    auto fs = std::make_unique<MemFs>();
    fs->add("/hello.txt", { 'h', 'i' });
    REQUIRE(v.Mount("/assets", std::move(fs)));
    CHECK(v.Contains("/assets/hello.txt"));
    CHECK_FALSE(v.Contains("/assets/missing.txt"));
    CHECK_FALSE(v.Contains("/other/hello.txt"));
}

TEST_CASE("Open returns a readable stream from the correct mount") {
    VFS  v;
    auto fs = std::make_unique<MemFs>();
    fs->add("/a.bin", { 1, 2, 3 });
    REQUIRE(v.Mount("/assets", std::move(fs)));
    auto s = v.Open("/assets/a.bin");
    REQUIRE(s != nullptr);
    CHECK(s->Size() == 3);
    CHECK(s->ReadUint8() == 1u);
}

TEST_CASE("Open missing path returns nullptr") {
    VFS  v;
    auto fs = std::make_unique<MemFs>();
    REQUIRE(v.Mount("/assets", std::move(fs)));
    CHECK(v.Open("/assets/missing.bin") == nullptr);
    CHECK(v.Open("/nonexistent_mount/foo") == nullptr);
}

TEST_CASE("Later mount takes priority (reverse iteration)") {
    VFS  v;
    auto a = std::make_unique<MemFs>();
    a->add("/file.bin", { 'A' });
    auto b = std::make_unique<MemFs>();
    b->add("/file.bin", { 'B' });
    REQUIRE(v.Mount("/assets", std::move(a)));
    REQUIRE(v.Mount("/assets", std::move(b)));

    auto s = v.Open("/assets/file.bin");
    REQUIRE(s != nullptr);
    CHECK(s->ReadUint8() == 'B'); // b mounted after a → wins
}

TEST_CASE("Unmount removes the last matching mount point") {
    VFS  v;
    auto fs = std::make_unique<MemFs>();
    fs->add("/x.bin", { 9 });
    REQUIRE(v.Mount("/assets", std::move(fs)));

    CHECK(v.Unmount("/assets"));
    CHECK(v.Open("/assets/x.bin") == nullptr);
    CHECK_FALSE(v.Unmount("/assets")); // second unmount fails
}

TEST_CASE("GetFileContent returns content or empty string") {
    VFS  v;
    auto fs = std::make_unique<MemFs>();
    fs->add("/hello.txt", { 'h', 'i' });
    REQUIRE(v.Mount("/assets", std::move(fs)));
    CHECK(GetFileContent(v, "/assets/hello.txt") == "hi");
    CHECK(GetFileContent(v, "/assets/missing.txt").empty());
}

TEST_CASE("MountedFs path helpers") {
    CHECK(VFS::MountedFs::CheckMountPoint("/a"));
    CHECK_FALSE(VFS::MountedFs::CheckMountPoint("/a/"));
    CHECK(VFS::MountedFs::InMountPoint("/assets", "/assets/file"));
    CHECK_FALSE(VFS::MountedFs::InMountPoint("/assets", "/other/file"));
    CHECK(VFS::MountedFs::GetPathInMount("/assets", "/assets/x/y") == "/x/y");
}

TEST_CASE("OpenW falls back to any mount covering the path (for writable fs)") {
    VFS  v;
    auto fs = std::make_unique<MemFs>(); // MemFs has no writable backing
    REQUIRE(v.Mount("/cache", std::move(fs)));
    // OpenW returns nullptr from MemFs regardless, but we exercise both
    // find_if passes (first that Contains, and second fallback).
    CHECK(v.OpenW("/cache/new.bin") == nullptr);
}

} // VFS
