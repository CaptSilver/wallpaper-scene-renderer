#include <doctest.h>

#include <cstdint>
#include <vector>

#include "Vulkan/TextureCacheDetail.hpp"

using wallpaper::vulkan::detail::parseQueryCapEnv;
using wallpaper::vulkan::detail::selectEvictionVictims;

// Pure-helper coverage for the TextureCache query-tex LRU + soft-cap added
// to bound m_query_texs growth on long-running dynamic scenes (puppet rigs,
// SceneScript setMaterial cycling, sprite-sheet pages).  The live integration
// (TextureCache::evictColdQueryTexs) needs a Device/VMA allocator and is
// exercised by the integration smoke in preflight.sh / sceneviewer.  Here we
// pin the eviction policy + env-parse contracts that the production code
// delegates to, so any regression surfaces in the unit suite first.
TEST_SUITE("TextureCache query-tex LRU policy") {

    TEST_CASE("evicts oldest non-persist entries above the soft cap") {
        // 6 non-persist entries with monotonically increasing lru_tick.
        // Cap = 4 → must evict 2 victims, the two with the lowest ticks
        // (indices 0 and 1).
        const std::vector<uint64_t> ticks    = { 100, 200, 300, 400, 500, 600 };
        const std::vector<uint8_t>  persists = { 0, 0, 0, 0, 0, 0 };
        const auto                  victims  = selectEvictionVictims(ticks, persists, /*cap=*/4);
        REQUIRE(victims.size() == 2);
        // Descending order — callers erase by index without renumbering.
        CHECK(victims[0] == 1);
        CHECK(victims[1] == 0);
    }

    TEST_CASE("persist=true entries are never evicted") {
        // 4 persist=true followed by 4 non-persist with newer ticks.  Cap=4
        // → 4 overflow, but only the non-persist tail is eligible → all 4
        // non-persist victims (indices 4-7) returned.
        const std::vector<uint64_t> ticks    = { 10, 20, 30, 40, 100, 200, 300, 400 };
        const std::vector<uint8_t>  persists = { 1, 1, 1, 1, 0, 0, 0, 0 };
        const auto                  victims  = selectEvictionVictims(ticks, persists, /*cap=*/4);
        REQUIRE(victims.size() == 4);
        // Descending order.
        CHECK(victims[0] == 7);
        CHECK(victims[1] == 6);
        CHECK(victims[2] == 5);
        CHECK(victims[3] == 4);
        // Persists (0-3) absent from the victim list.
        for (auto v : victims) CHECK(v >= 4);
    }

    TEST_CASE("hit-style bump on an existing entry rescues it from eviction") {
        // A's tick (100) was the oldest, then a hypothetical hit bumped it
        // to 999 (above all peers).  B (tick 200) becomes the new LRU and
        // must be evicted instead of A.
        const std::vector<uint64_t> ticks    = { 999, 200, 300, 400 };
        const std::vector<uint8_t>  persists = { 0, 0, 0, 0 };
        const auto                  victims  = selectEvictionVictims(ticks, persists, /*cap=*/3);
        REQUIRE(victims.size() == 1);
        CHECK(victims[0] == 1); // B is the new LRU.
    }

    TEST_CASE("pool at or below cap returns no victims") {
        const std::vector<uint64_t> ticks    = { 100, 200, 300 };
        const std::vector<uint8_t>  persists = { 0, 0, 0 };
        CHECK(selectEvictionVictims(ticks, persists, /*cap=*/4).empty());
        CHECK(selectEvictionVictims(ticks, persists, /*cap=*/3).empty());
    }

    TEST_CASE("all-persist pool over-cap returns no victims (soft cap honoured)") {
        // No non-persist eligible → eviction is a no-op; the pool stays
        // above the cap intentionally (the cap is soft, persist=true
        // entries are RG-essential).
        const std::vector<uint64_t> ticks    = { 100, 200, 300, 400 };
        const std::vector<uint8_t>  persists = { 1, 1, 1, 1 };
        CHECK(selectEvictionVictims(ticks, persists, /*cap=*/2).empty());
    }

    TEST_CASE("mismatched view sizes return empty (defensive)") {
        const std::vector<uint64_t> ticks    = { 100, 200, 300, 400 };
        const std::vector<uint8_t>  persists = { 0, 0 };
        CHECK(selectEvictionVictims(ticks, persists, /*cap=*/1).empty());
    }
}

TEST_SUITE("TextureCache query-cap env parse") {

    TEST_CASE("null or empty env returns the default") {
        CHECK(parseQueryCapEnv(nullptr, /*default=*/64) == 64u);
        CHECK(parseQueryCapEnv("", /*default=*/64) == 64u);
    }

    TEST_CASE("a valid in-range integer is accepted") {
        CHECK(parseQueryCapEnv("16", /*default=*/64) == 16u);
        CHECK(parseQueryCapEnv("256", /*default=*/64) == 256u);
        CHECK(parseQueryCapEnv("4096", /*default=*/64) == 4096u);
    }

    TEST_CASE("out-of-range integers fall back to the default") {
        // Below min=8.
        CHECK(parseQueryCapEnv("0", /*default=*/64) == 64u);
        CHECK(parseQueryCapEnv("7", /*default=*/64) == 64u);
        // Above max=4096.
        CHECK(parseQueryCapEnv("4097", /*default=*/64) == 64u);
        CHECK(parseQueryCapEnv("99999", /*default=*/64) == 64u);
    }

    TEST_CASE("malformed values fall back to the default") {
        // Non-numeric.
        CHECK(parseQueryCapEnv("garbage", /*default=*/64) == 64u);
        CHECK(parseQueryCapEnv("abc", /*default=*/64) == 64u);
        // Trailing garbage after a number — strtoul accepts the prefix,
        // but parseQueryCapEnv requires the whole string to parse.
        CHECK(parseQueryCapEnv("64abc", /*default=*/64) == 64u);
        CHECK(parseQueryCapEnv("64 ", /*default=*/64) == 64u);
    }

    TEST_CASE("custom bounds are honoured") {
        // min=2, max=16: 8 accepted, 1 rejected, 32 rejected.
        CHECK(parseQueryCapEnv("8", /*default=*/4, /*min=*/2, /*max=*/16) == 8u);
        CHECK(parseQueryCapEnv("1", /*default=*/4, /*min=*/2, /*max=*/16) == 4u);
        CHECK(parseQueryCapEnv("32", /*default=*/4, /*min=*/2, /*max=*/16) == 4u);
    }
}
