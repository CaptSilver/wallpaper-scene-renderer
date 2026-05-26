#include "PipelineCacheIO.hpp"

#include "Utils/Logging.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>

namespace wallpaper::vulkan::pipeline_cache_io
{

std::filesystem::path PathFromEnv() {
    const char*           xdg = std::getenv("XDG_CACHE_HOME");
    std::filesystem::path base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (! home || ! *home) return {};
        base = std::filesystem::path(home) / ".cache";
    }
    return base / "wallpaper-scene-renderer" / "pipeline.cache";
}

bool HeaderMatches(const std::vector<char>& blob, const VkPhysicalDeviceProperties& props) {
    if (blob.size() < 32) return false;
    uint32_t header_len, header_ver, vendor, device;
    std::memcpy(&header_len, blob.data() + 0, 4);
    std::memcpy(&header_ver, blob.data() + 4, 4);
    std::memcpy(&vendor, blob.data() + 8, 4);
    std::memcpy(&device, blob.data() + 12, 4);
    if (header_len != 32) return false;
    if (header_ver != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) return false;
    if (vendor != props.vendorID) return false;
    if (device != props.deviceID) return false;
    if (std::memcmp(blob.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) != 0) return false;
    return true;
}

std::vector<char> Read(const std::filesystem::path& path) {
    std::error_code ec;
    if (! std::filesystem::exists(path, ec) || ec) return {};
    std::ifstream f(path, std::ios::binary);
    if (! f) return {};
    f.seekg(0, std::ios::end);
    const auto sz = f.tellg();
    if (sz <= 0) return {};
    f.seekg(0, std::ios::beg);
    std::vector<char> blob(static_cast<std::size_t>(sz));
    if (! f.read(blob.data(), sz)) return {};
    return blob;
}

bool Write(const std::filesystem::path& path, const void* data, std::size_t bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        LOG_ERROR("pipeline cache: mkdir(%s) failed: %s",
                  path.parent_path().c_str(),
                  ec.message().c_str());
        return false;
    }
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (! f) return false;
        f.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        if (! f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace wallpaper::vulkan::pipeline_cache_io
