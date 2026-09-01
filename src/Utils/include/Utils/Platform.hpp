#pragma once
#include <cstdlib>
#include <string_view>
#include <filesystem>
#include "Core/StringHelper.hpp"

namespace wallpaper
{
namespace platform
{

// Names inside the user cache root that the renderer owns. They live here,
// in a header with no Qt/Vulkan dependencies, because the KDE plugin has to
// know them too: it garbage-collects this cache, and a second copy of the
// strings on that side is what let the two halves drift apart before.
//
//   <cache>/wescene-renderer/            scene cache (compiled shaders, ...)
//   <cache>/wallpaper-scene-renderer/    Vulkan pipeline cache + diagnostics
inline constexpr std::string_view kRendererCacheDir { "wescene-renderer" };
inline constexpr std::string_view kPipelineCacheDir { "wallpaper-scene-renderer" };

// SceneScript's localStorage. Written by the scene itself, never regenerated
// — a cache sweep that deletes these throws away the user's save data, so
// every GC path must skip them by basename.
//   <cache>/wescene-renderer/localstorage_global.json
//   <cache>/wescene-renderer/<sceneId>/localstorage.json
inline constexpr std::string_view kLocalStorageGlobalFile { "localstorage_global.json" };
inline constexpr std::string_view kLocalStorageSceneFile { "localstorage.json" };

inline std::filesystem::path GetCachePath(std::string_view name) {
    using namespace std::filesystem;

    path             p_cache;
    std::string_view home = sview_nullsafe(std::getenv("HOME"));
    if (! home.empty()) {
        std::string_view cache = sview_nullsafe(std::getenv("XDG_CACHE_HOME"));
        if (cache.empty())
            p_cache = path(home) / ".cache";
        else
            p_cache = path(cache);
    }
    return p_cache / name;
}

} // namespace platform
} // namespace wallpaper
