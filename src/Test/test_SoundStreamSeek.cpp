#include <doctest.h>
#include "Audio/SoundManager.h"
#include "Fs/MemBinaryStream.h"

#include <vector>
#include <cstdint>
#include <cmath>

using namespace wallpaper;

namespace
{

// Generate a minimal mono 16-bit PCM WAV file in memory
std::vector<uint8_t> MakeWavData(uint32_t numSamples, uint32_t sampleRate = 44100) {
    const uint16_t channels      = 1;
    const uint16_t bitsPerSample = 16;
    const uint32_t dataSize      = numSamples * channels * (bitsPerSample / 8);

    std::vector<uint8_t> buf;
    buf.reserve(44 + dataSize);

    auto writeU32 = [&](uint32_t v) {
        buf.push_back((uint8_t)(v));
        buf.push_back((uint8_t)(v >> 8));
        buf.push_back((uint8_t)(v >> 16));
        buf.push_back((uint8_t)(v >> 24));
    };
    auto writeU16 = [&](uint16_t v) {
        buf.push_back((uint8_t)(v));
        buf.push_back((uint8_t)(v >> 8));
    };
    auto writeTag = [&](const char* s) {
        for (int i = 0; i < 4; i++) buf.push_back((uint8_t)s[i]);
    };

    writeTag("RIFF");
    writeU32(36 + dataSize);
    writeTag("WAVE");
    writeTag("fmt ");
    writeU32(16);
    writeU16(1); // PCM
    writeU16(channels);
    writeU32(sampleRate);
    writeU32(sampleRate * channels * (bitsPerSample / 8));
    writeU16(channels * (bitsPerSample / 8));
    writeU16(bitsPerSample);
    writeTag("data");
    writeU32(dataSize);

    // Triangle wave samples (non-zero, deterministic)
    for (uint32_t i = 0; i < numSamples; i++) {
        int16_t sample = (int16_t)(10000.0 * (2.0 * std::fabs((double)(i % 100) / 50.0 - 1.0) - 1.0));
        writeU16((uint16_t)sample);
    }
    return buf;
}

} // namespace

// Looping is handled at the WPSoundStream layer by always recreating the decoder
// (Switch()) at EOF — not by seeking. These tests lock in the SoundStream contract
// that callers rely on: partial-read frames at EOF are preserved, and a fresh
// SoundStream produces identical data to a prior fresh one on the same source
// (validating the Switch-based loop strategy).
TEST_SUITE("SoundStream EOF + loop-via-Switch") {

TEST_CASE("REGRESSION: partial read at EOF preserves tail frames") {
    // Bug: ma_decoder_read_pcm_frames returns MA_AT_END with readed>0 for the
    // final chunk of a file. The wrapper used to return 0 on any non-SUCCESS,
    // silently dropping the tail. Fix: return readed on both MA_SUCCESS and
    // MA_AT_END. This test drives the decoder past EOF and asserts we consumed
    // every source sample before the stream reports 0.
    const uint32_t numSamples = 1000;
    auto wavData   = MakeWavData(numSamples);
    auto memStream = std::make_shared<fs::MemBinaryStream>(std::move(wavData));

    audio::SoundStream::Desc desc { .channels = 1, .sampleRate = 44100 };
    auto stream = audio::CreateSoundStream(memStream, desc);
    REQUIRE(stream);

    // Request in a chunk size that doesn't evenly divide the file length,
    // so the final read straddles EOF.
    const uint32_t chunk = 384;
    std::vector<float> buf(chunk);
    uint64_t totalRead = 0;
    uint64_t reads;
    do {
        reads = stream->NextPcmData(buf.data(), chunk);
        totalRead += reads;
    } while (reads > 0);

    // Must have consumed every source sample — no tail drop.
    CHECK(totalRead == numSamples);
}

TEST_CASE("Fresh SoundStream reproduces data (Switch-based loop strategy)") {
    // WPSoundStream loops by calling Switch(), which constructs a new SoundStream
    // from the same source. This test verifies that two freshly-created streams
    // over the same underlying bytes produce identical PCM output — the invariant
    // the loop strategy depends on.
    auto wavData = MakeWavData(512);

    auto run = [&](std::vector<float>& out) {
        auto memStream = std::make_shared<fs::MemBinaryStream>(
            std::vector<uint8_t>(wavData));
        audio::SoundStream::Desc desc { .channels = 1, .sampleRate = 44100 };
        auto stream = audio::CreateSoundStream(memStream, desc);
        REQUIRE(stream);
        std::vector<float> tmp(256);
        uint64_t r;
        while ((r = stream->NextPcmData(tmp.data(), (uint32_t)tmp.size())) > 0) {
            out.insert(out.end(), tmp.begin(), tmp.begin() + r);
        }
    };

    std::vector<float> first, second;
    run(first);
    run(second);

    REQUIRE(first.size() == second.size());
    REQUIRE(first.size() > 0);
    for (size_t i = 0; i < first.size(); i++) {
        CHECK(first[i] == doctest::Approx(second[i]));
    }
}

TEST_CASE("Multiple fresh streams in sequence (simulates many loop iterations)") {
    // Simulates what WPSoundStream does over a long session: Switch() many times.
    // No shared state between streams, so this must remain deterministic forever.
    auto wavData = MakeWavData(128);

    for (int iter = 0; iter < 10; iter++) {
        auto memStream = std::make_shared<fs::MemBinaryStream>(
            std::vector<uint8_t>(wavData));
        audio::SoundStream::Desc desc { .channels = 1, .sampleRate = 44100 };
        auto stream = audio::CreateSoundStream(memStream, desc);
        REQUIRE(stream);

        std::vector<float> buf(256);
        uint64_t totalRead = 0;
        uint64_t r;
        while ((r = stream->NextPcmData(buf.data(), (uint32_t)buf.size())) > 0) {
            totalRead += r;
        }
        CHECK(totalRead == 128);
    }
}

} // TEST_SUITE
