#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <unordered_map>

namespace wallpaper
{

// Gate for the SceneScript world-transform cache.  The render thread
// rebuilds m_layer_world_cache at the end of every drawFrame so the GUI-thread
// bridge (thisLayer.getTransformMatrix() / hit-test) can read live world
// matrices.  Most scenes never read it, so the producer should not do the
// O(named-nodes) UpdateTrans() walk unless a consumer has appeared.
//
// markWorldCacheNeeded latches the flag true on the first reader access.
// worldCacheShouldRebuild reports whether the per-frame rebuild should run.
// Identity is the documented fallback for an unknown / not-yet-cached id, so a
// scene that hit-tests on frame 0 (before the flag propagates one frame) sees
// identity for one frame — matching existing not-yet-cached behavior.
inline void markWorldCacheNeeded(std::atomic<bool>& flag) {
    flag.store(true, std::memory_order_relaxed);
}
inline bool worldCacheShouldRebuild(const std::atomic<bool>& flag) {
    return flag.load(std::memory_order_relaxed);
}

// The identity matrix the reader returns for an uncached id (column-major).
inline std::array<float, 16> worldCacheIdentity() {
    return { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
}

// in-place cache update.  Reuse the existing bucket node via
// operator[] instead of clear()+emplace(), so the steady-state node set (stable
// between reloads) never reallocates and a concurrent reader never sees a
// transient gap for a known id.  Returns the assigned value.
inline const std::array<float, 16>&
worldCacheAssign(std::unordered_map<int32_t, std::array<float, 16>>& cache, int32_t id,
                 const std::array<float, 16>& arr) {
    return cache[id] = arr; // assign in place; no rehash, no node realloc
}

} // namespace wallpaper
