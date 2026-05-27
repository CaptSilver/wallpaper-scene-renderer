#pragma once
#include <atomic>

namespace wallpaper
{

// Gate for the per-node sprite playback snapshot.  WPShaderValueUpdater
// publishes the snapshot every frame so the JS bridge
// thisLayer.getTextureAnimation() can read frame count / duration /
// current frame / manual-pin state.  Most wallpapers never call
// getTextureAnimation(), so the producer skips the publish unless a
// consumer has appeared.
//
// markSpriteSnapshotNeeded latches the flag true on the first reader
// access.  spriteSnapshotShouldPublish reports whether the per-frame
// publication should run.
//
// The flag is sticky-on: once a consumer has appeared in this scene, it
// stays on for the scene's lifetime.  The very first read (before the
// producer observes the flag) returns the default-constructed
// NodeSpriteSnapshot — matching the "no entry / not yet published"
// fallback the bridge already handles (SceneWallpaper::
// getLayerSpriteSnapshot returns {} on cache miss).
//
// One-frame propagation delay is harmless: a script that reads sprite
// state on its first tick sees default-zero values once, then live
// values on the next tick.  Mirrors WorldCacheGate.h.
inline void markSpriteSnapshotNeeded(std::atomic<bool>& flag) {
    flag.store(true, std::memory_order_relaxed);
}
inline bool spriteSnapshotShouldPublish(const std::atomic<bool>& flag) {
    return flag.load(std::memory_order_relaxed);
}

} // namespace wallpaper
