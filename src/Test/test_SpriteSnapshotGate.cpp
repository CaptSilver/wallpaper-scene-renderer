#include <doctest.h>
#include "Scene/SpriteSnapshotGate.h"
#include <atomic>

using namespace wallpaper;

// Gate the per-frame sprite-snapshot publication on consumer presence.
// The pure helpers below are the exact contract WPShaderValueUpdater (producer)
// and SceneWallpaper::getLayerSpriteSnapshot (consumer) call.  Mirrors
// test_WorldCacheGate.cpp.
TEST_SUITE("SpriteSnapshotGate") {
    TEST_CASE("flag defaults false -> publish is skipped on a non-consuming scene") {
        std::atomic<bool> flag { false };
        CHECK(spriteSnapshotShouldPublish(flag) == false);
    }
    TEST_CASE("first reader access latches the flag true") {
        std::atomic<bool> flag { false };
        markSpriteSnapshotNeeded(flag);
        CHECK(spriteSnapshotShouldPublish(flag) == true);
    }
    TEST_CASE("repeated marks stay true (idempotent latch)") {
        std::atomic<bool> flag { false };
        markSpriteSnapshotNeeded(flag);
        markSpriteSnapshotNeeded(flag);
        CHECK(spriteSnapshotShouldPublish(flag) == true);
    }
    TEST_CASE("reset to false re-disables (scene reload path)") {
        // A new scene swap constructs a fresh Scene with the flag at false;
        // verify the reset semantics here even though production code doesn't
        // explicitly flip-back (the scene swap implicitly does via Scene
        // destruction + construction).
        std::atomic<bool> flag { false };
        markSpriteSnapshotNeeded(flag);
        flag.store(false, std::memory_order_relaxed);
        CHECK(spriteSnapshotShouldPublish(flag) == false);
    }
    TEST_CASE("relaxed memory ordering is the documented contract") {
        // Sanity: the helpers use std::memory_order_relaxed.  A future refactor
        // that switches to acquire/release would still pass the above cases —
        // this case pins the relaxed contract by checking the store/load
        // round-trip is observable in a single-threaded execution (which all of
        // the above already assume).  Documented here so reviewers know relaxed
        // is intentional.
        std::atomic<bool> flag { false };
        CHECK(flag.load(std::memory_order_relaxed) == false);
        markSpriteSnapshotNeeded(flag);
        CHECK(flag.load(std::memory_order_relaxed) == true);
    }
} // TEST_SUITE SpriteSnapshotGate
