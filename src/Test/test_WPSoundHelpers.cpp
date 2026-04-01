#include <doctest.h>
#include "WPSoundHelpers.h"

#include <set>

using wallpaper::NextSoundIndex;
using wallpaper::RandomDelaySamples;

TEST_SUITE("NextSoundIndex") {

TEST_CASE("single track always returns 0") {
    std::mt19937 rng(42);
    CHECK(NextSoundIndex(0, 1, false, rng) == 0);
    CHECK(NextSoundIndex(0, 1, true, rng) == 0);
}

TEST_CASE("sequential mode cycles through tracks") {
    std::mt19937 rng(42);
    CHECK(NextSoundIndex(0, 3, false, rng) == 1);
    CHECK(NextSoundIndex(1, 3, false, rng) == 2);
    CHECK(NextSoundIndex(2, 3, false, rng) == 0); // wraps
}

TEST_CASE("sequential mode wraps at boundary") {
    std::mt19937 rng(42);
    CHECK(NextSoundIndex(4, 5, false, rng) == 0);
    CHECK(NextSoundIndex(9, 10, false, rng) == 0);
}

TEST_CASE("random mode never returns current index") {
    std::mt19937 rng(123);
    // Run many iterations to verify we never get the same index back
    for (int trial = 0; trial < 200; trial++) {
        uint32_t cur = (uint32_t)(trial % 5);
        uint32_t next = NextSoundIndex(cur, 5, true, rng);
        CHECK(next != cur);
        CHECK(next < 5);
    }
}

TEST_CASE("random mode covers all other tracks") {
    std::mt19937 rng(42);
    // With 3 tracks and current=1, we should eventually see both 0 and 2
    std::set<uint32_t> seen;
    for (int i = 0; i < 100; i++) {
        uint32_t next = NextSoundIndex(1, 3, true, rng);
        seen.insert(next);
    }
    CHECK(seen.count(0) == 1);
    CHECK(seen.count(2) == 1);
    CHECK(seen.count(1) == 0); // never returns current
}

TEST_CASE("random mode with 2 tracks always returns the other") {
    std::mt19937 rng(99);
    for (int i = 0; i < 20; i++) {
        CHECK(NextSoundIndex(0, 2, true, rng) == 1);
        CHECK(NextSoundIndex(1, 2, true, rng) == 0);
    }
}

TEST_CASE("sequential mode with 2 tracks alternates") {
    std::mt19937 rng(42);
    CHECK(NextSoundIndex(0, 2, false, rng) == 1);
    CHECK(NextSoundIndex(1, 2, false, rng) == 0);
}

} // TEST_SUITE NextSoundIndex

TEST_SUITE("RandomDelaySamples") {

TEST_CASE("zero times returns zero") {
    std::mt19937 rng(42);
    CHECK(RandomDelaySamples(0, 0, 44100, rng) == 0);
}

TEST_CASE("negative times returns zero") {
    std::mt19937 rng(42);
    CHECK(RandomDelaySamples(-1, -1, 44100, rng) == 0);
    CHECK(RandomDelaySamples(-5, 0, 44100, rng) == 0);
}

TEST_CASE("positive range returns non-zero samples") {
    std::mt19937 rng(42);
    uint64_t samples = RandomDelaySamples(1.0f, 3.0f, 44100, rng);
    CHECK(samples >= 44100);   // at least 1 second
    CHECK(samples <= 132300);  // at most 3 seconds
}

TEST_CASE("equal min and max returns exact delay") {
    std::mt19937 rng(42);
    uint64_t samples = RandomDelaySamples(2.0f, 2.0f, 44100, rng);
    CHECK(samples == (uint64_t)(2.0f * 44100));
}

TEST_CASE("min > max clamps to min") {
    std::mt19937 rng(42);
    // When min > max, hi = max(lo, maxtime) = lo, so it becomes [lo, lo]
    uint64_t samples = RandomDelaySamples(5.0f, 2.0f, 44100, rng);
    CHECK(samples == (uint64_t)(5.0f * 44100));
}

TEST_CASE("positive mintime with negative maxtime produces non-zero delay") {
    // Kills mutant: mintime <= 0 → mintime > 0
    // Original: 0.5<=0 is false → falls through → delay > 0
    // Mutant:   0.5>0 is true  → returns 0 (early exit with maxtime<0)
    std::mt19937 rng(42);
    uint64_t samples = RandomDelaySamples(0.5f, -1.0f, 44100, rng);
    CHECK(samples > 0);
}

TEST_CASE("zero mintime with positive maxtime produces non-zero delay") {
    // Kills mutant: maxtime <= 0 → maxtime > 0 and maxtime <= 0 → maxtime < 0
    // mintime=0, maxtime=1: original: 0<=0 && 1<=0 = false → falls through
    std::mt19937 rng(42);
    uint64_t samples = RandomDelaySamples(0.0f, 1.0f, 44100, rng);
    CHECK(samples > 0);
}

TEST_CASE("negative mintime with positive maxtime produces non-zero delay") {
    std::mt19937 rng(42);
    uint64_t samples = RandomDelaySamples(-1.0f, 1.0f, 44100, rng);
    CHECK(samples > 0);
}

TEST_CASE("sample rate affects output") {
    std::mt19937 rng1(42), rng2(42);
    uint64_t s1 = RandomDelaySamples(1.0f, 1.0f, 44100, rng1);
    uint64_t s2 = RandomDelaySamples(1.0f, 1.0f, 48000, rng2);
    CHECK(s1 == 44100);
    CHECK(s2 == 48000);
}

} // TEST_SUITE RandomDelaySamples
