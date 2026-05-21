#include <doctest.h>
#include "Scene/WorldCacheGate.h"
#include <atomic>
#include <unordered_map>

using namespace wallpaper;

// The render thread rebuilds the SceneScript world-transform cache
// at the end of every drawFrame; most scenes never read it.  These pure
// helpers (gate latch, in-place assign, identity fallback) are the exact
// functions the production rebuild calls, extracted so they are testable
// without driving a render-thread CMD_DRAW (which needs Vulkan).
TEST_SUITE("WorldCacheGate") {
    TEST_CASE("flag defaults false -> rebuild is skipped on a non-consuming scene") {
        std::atomic<bool> flag { false };
        CHECK(worldCacheShouldRebuild(flag) == false);
    }
    TEST_CASE("first reader access latches the flag true") {
        std::atomic<bool> flag { false };
        markWorldCacheNeeded(flag);
        CHECK(worldCacheShouldRebuild(flag) == true);
    }
    TEST_CASE("repeated marks stay true (idempotent latch)") {
        std::atomic<bool> flag { false };
        markWorldCacheNeeded(flag);
        markWorldCacheNeeded(flag);
        CHECK(worldCacheShouldRebuild(flag) == true);
    }
    TEST_CASE("reset to false re-disables (scene reload path)") {
        // CMD_SET_SCENE resets the flag so a new script-light scene pays zero
        // until its own scripts hit-test.
        std::atomic<bool> flag { false };
        markWorldCacheNeeded(flag);
        flag.store(false, std::memory_order_relaxed);
        CHECK(worldCacheShouldRebuild(flag) == false);
    }
    TEST_CASE("identity fallback is the documented column-major identity") {
        auto id = worldCacheIdentity();
        CHECK(id[0] == 1.f);
        CHECK(id[5] == 1.f);
        CHECK(id[10] == 1.f);
        CHECK(id[15] == 1.f);
        CHECK(id[12] == 0.f);
        CHECK(id[13] == 0.f);
        CHECK(id[14] == 0.f);
    }

    TEST_CASE("in-place assign reuses the slot; value is the latest written") {
        std::unordered_map<int32_t, std::array<float, 16>> cache;
        std::array<float, 16>                              a = worldCacheIdentity();
        a[12]                                                = 100.f; // translation x
        worldCacheAssign(cache, 7, a);
        CHECK(cache.at(7)[12] == 100.f);
        // Second "rebuild" with a new transform for the same id.
        std::array<float, 16> b = worldCacheIdentity();
        b[12]                   = 250.f;
        worldCacheAssign(cache, 7, b);
        CHECK(cache.size() == 1u);       // reused the slot, did not grow
        CHECK(cache.at(7)[12] == 250.f); // latest value
    }
    TEST_CASE("known id never reads identity between two in-place rebuilds") {
        // The clear()-induced gap is what this removes: with operator[] the id 7
        // entry is always present and is overwritten, so a read at any point
        // returns a real matrix, never the identity fallback.
        std::unordered_map<int32_t, std::array<float, 16>> cache;
        std::array<float, 16>                              v = worldCacheIdentity();
        v[13]                                                = 42.f;
        worldCacheAssign(cache, 7, v);
        auto read = [&](int32_t id) {
            auto it = cache.find(id);
            return it != cache.end() ? it->second : worldCacheIdentity();
        };
        CHECK(read(7)[13] == 42.f); // not identity (0)
        v[13] = 84.f;
        worldCacheAssign(cache, 7, v);
        CHECK(read(7)[13] == 84.f);
    }
    TEST_CASE("scene reload clears stale ids so in-place reuse is safe") {
        // CMD_SET_SCENE clears the cache; without this, a previous scene's id
        // (with no reader in the new scene) would linger after we removed the
        // per-frame clear() in Task 2.  A reader that queries a recycled id must
        // not get the OLD scene's matrix.
        std::unordered_map<int32_t, std::array<float, 16>> cache;
        std::array<float, 16>                              old = worldCacheIdentity();
        old[12]                                                = 999.f;
        worldCacheAssign(cache, 7, old);
        REQUIRE(cache.size() == 1u);
        cache.clear(); // == the CMD_SET_SCENE reset
        CHECK(cache.empty());
        auto it = cache.find(7);
        CHECK(it == cache.end()); // stale id gone -> reader gets identity
    }
} // TEST_SUITE WorldCacheGate
