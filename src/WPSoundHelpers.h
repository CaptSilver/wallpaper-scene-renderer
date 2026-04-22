#pragma once
#include <cstdint>
#include <random>
#include <algorithm>

namespace wallpaper
{

/// Pick the next track index for sound playback.
/// @param curIndex   current track index
/// @param trackCount total number of tracks
/// @param random     true for random mode, false for sequential
/// @param rng        random engine (only used if random=true)
/// @return next track index in [0, trackCount)
inline uint32_t NextSoundIndex(uint32_t curIndex, uint32_t trackCount, bool random,
                               std::mt19937& rng) {
    if (trackCount <= 1) return 0;

    if (random) {
        // Pick a random track different from the current one
        std::uniform_int_distribution<uint32_t> dist(0, trackCount - 2);
        uint32_t                                pick = dist(rng);
        if (pick >= curIndex) pick++;
        return pick;
    }

    uint32_t next = curIndex + 1;
    return next >= trackCount ? 0 : next;
}

/// Compute a random delay in sample frames for gaps between random tracks.
/// @param mintime    minimum delay in seconds
/// @param maxtime    maximum delay in seconds
/// @param sampleRate audio sample rate (e.g. 44100)
/// @param rng        random engine
/// @return number of silence frames to insert
inline uint64_t RandomDelaySamples(float mintime, float maxtime, uint32_t sampleRate,
                                   std::mt19937& rng) {
    float                                 lo = std::max(0.0f, mintime);
    float                                 hi = std::max(lo, maxtime);
    std::uniform_real_distribution<float> dist(lo, hi);
    float                                 delaySec = dist(rng);
    return (uint64_t)(delaySec * (float)sampleRate);
}

} // namespace wallpaper
