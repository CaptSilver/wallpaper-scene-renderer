#include <doctest.h>

#include "Vulkan/PipelineCacheIO.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using wallpaper::vulkan::pipeline_cache_io::HeaderMatches;
using wallpaper::vulkan::pipeline_cache_io::PathFromEnv;
using wallpaper::vulkan::pipeline_cache_io::Read;
using wallpaper::vulkan::pipeline_cache_io::Write;

namespace
{

struct EnvScope {
    const char* key;
    std::string saved;
    bool        had;
    EnvScope(const char* k, const char* v): key(k) {
        const char* prev = std::getenv(k);
        had              = prev != nullptr;
        if (had) saved = prev;
        if (v)
            setenv(k, v, 1);
        else
            unsetenv(k);
    }
    ~EnvScope() {
        if (had)
            setenv(key, saved.c_str(), 1);
        else
            unsetenv(key);
    }
};

VkPhysicalDeviceProperties MakeProps(uint32_t vendor, uint32_t device,
                                     std::array<uint8_t, VK_UUID_SIZE> uuid) {
    VkPhysicalDeviceProperties p {};
    p.vendorID = vendor;
    p.deviceID = device;
    std::memcpy(p.pipelineCacheUUID, uuid.data(), VK_UUID_SIZE);
    return p;
}

std::vector<char> MakeHeader(uint32_t header_len, uint32_t header_ver, uint32_t vendor,
                             uint32_t device, std::array<uint8_t, VK_UUID_SIZE> uuid,
                             size_t payload = 0) {
    std::vector<char> b(32 + payload, '\0');
    std::memcpy(b.data() + 0, &header_len, 4);
    std::memcpy(b.data() + 4, &header_ver, 4);
    std::memcpy(b.data() + 8, &vendor, 4);
    std::memcpy(b.data() + 12, &device, 4);
    std::memcpy(b.data() + 16, uuid.data(), VK_UUID_SIZE);
    return b;
}

} // namespace

TEST_SUITE_BEGIN("PipelineCacheIO");

TEST_CASE("PathFromEnv prefers XDG_CACHE_HOME when set") {
    EnvScope xdg("XDG_CACHE_HOME", "/tmp/wek-cache-test");
    EnvScope home("HOME", "/home/should-not-be-used");
    const auto p = PathFromEnv();
    CHECK(p.string() == "/tmp/wek-cache-test/wallpaper-scene-renderer/pipeline.cache");
}

TEST_CASE("PathFromEnv falls back to $HOME/.cache when XDG_CACHE_HOME unset") {
    EnvScope xdg("XDG_CACHE_HOME", nullptr);
    EnvScope home("HOME", "/tmp/wek-home-test");
    const auto p = PathFromEnv();
    CHECK(p.string() == "/tmp/wek-home-test/.cache/wallpaper-scene-renderer/pipeline.cache");
}

TEST_CASE("PathFromEnv returns empty when XDG_CACHE_HOME and HOME both missing") {
    EnvScope xdg("XDG_CACHE_HOME", nullptr);
    EnvScope home("HOME", nullptr);
    const auto p = PathFromEnv();
    CHECK(p.empty());
}

TEST_CASE("PathFromEnv treats empty XDG_CACHE_HOME like unset") {
    EnvScope xdg("XDG_CACHE_HOME", "");
    EnvScope home("HOME", "/tmp/wek-home-test");
    const auto p = PathFromEnv();
    CHECK(p.string() == "/tmp/wek-home-test/.cache/wallpaper-scene-renderer/pipeline.cache");
}

TEST_CASE("HeaderMatches accepts a well-formed matching header") {
    std::array<uint8_t, VK_UUID_SIZE> uuid {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
    };
    auto       props = MakeProps(0x1002, 0x73DF, uuid);
    const auto blob  = MakeHeader(32, VK_PIPELINE_CACHE_HEADER_VERSION_ONE, 0x1002, 0x73DF, uuid);
    CHECK(HeaderMatches(blob, props));
}

TEST_CASE("HeaderMatches rejects blob shorter than the header") {
    std::array<uint8_t, VK_UUID_SIZE> uuid {};
    auto                              props = MakeProps(0x1002, 0x73DF, uuid);
    std::vector<char>                 too_short(20, '\0');
    CHECK_FALSE(HeaderMatches(too_short, props));
    std::vector<char> empty;
    CHECK_FALSE(HeaderMatches(empty, props));
}

TEST_CASE("HeaderMatches rejects on header length mismatch") {
    std::array<uint8_t, VK_UUID_SIZE> uuid {};
    auto       props = MakeProps(0x1002, 0x73DF, uuid);
    const auto blob  = MakeHeader(48, VK_PIPELINE_CACHE_HEADER_VERSION_ONE, 0x1002, 0x73DF, uuid);
    CHECK_FALSE(HeaderMatches(blob, props));
}

TEST_CASE("HeaderMatches rejects on header version mismatch") {
    std::array<uint8_t, VK_UUID_SIZE> uuid {};
    auto                              props = MakeProps(0x1002, 0x73DF, uuid);
    const auto                        blob  = MakeHeader(32, 0xFFFFFFFF, 0x1002, 0x73DF, uuid);
    CHECK_FALSE(HeaderMatches(blob, props));
}

TEST_CASE("HeaderMatches rejects on vendor / device / UUID mismatch") {
    std::array<uint8_t, VK_UUID_SIZE> uuid {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
    };
    auto props = MakeProps(0x1002, 0x73DF, uuid);
    SUBCASE("vendor differs") {
        const auto blob =
            MakeHeader(32, VK_PIPELINE_CACHE_HEADER_VERSION_ONE, 0x10DE, 0x73DF, uuid);
        CHECK_FALSE(HeaderMatches(blob, props));
    }
    SUBCASE("device differs") {
        const auto blob =
            MakeHeader(32, VK_PIPELINE_CACHE_HEADER_VERSION_ONE, 0x1002, 0x9999, uuid);
        CHECK_FALSE(HeaderMatches(blob, props));
    }
    SUBCASE("UUID differs by one byte") {
        auto other_uuid = uuid;
        other_uuid[7] ^= 0xFF;
        const auto blob =
            MakeHeader(32, VK_PIPELINE_CACHE_HEADER_VERSION_ONE, 0x1002, 0x73DF, other_uuid);
        CHECK_FALSE(HeaderMatches(blob, props));
    }
}

TEST_CASE("Write then Read round-trips bytes through a real tmpfile") {
    const auto base = std::filesystem::temp_directory_path() / "wek-pcache-test";
    std::filesystem::remove_all(base);
    const auto path = base / "nested" / "dir" / "pipeline.cache";
    const std::array<unsigned char, 9> bytes { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE, 0x00 };
    REQUIRE(Write(path, bytes.data(), bytes.size()));
    const auto blob = Read(path);
    REQUIRE(blob.size() == bytes.size());
    CHECK(std::memcmp(blob.data(), bytes.data(), bytes.size()) == 0);
    std::filesystem::remove_all(base);
}

TEST_CASE("Read returns empty on missing file") {
    const auto path = std::filesystem::temp_directory_path() / "wek-pcache-not-here.cache";
    std::filesystem::remove(path);
    const auto blob = Read(path);
    CHECK(blob.empty());
}

TEST_CASE("Write leaves no .tmp leftover on success") {
    const auto base = std::filesystem::temp_directory_path() / "wek-pcache-test2";
    std::filesystem::remove_all(base);
    const auto path = base / "pipeline.cache";
    const std::array<unsigned char, 4> bytes { 1, 2, 3, 4 };
    REQUIRE(Write(path, bytes.data(), bytes.size()));
    auto tmp = path;
    tmp += ".tmp";
    CHECK_FALSE(std::filesystem::exists(tmp));
    std::filesystem::remove_all(base);
}

TEST_SUITE_END();
