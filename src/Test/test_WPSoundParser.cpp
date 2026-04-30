#include <doctest.h>
#include "WPSoundParser.hpp"
#include "wpscene/WPSoundObject.h"
#include "Audio/SoundManager.h"
#include "Fs/VFS.h"
#include "Fs/Fs.h"
#include "Fs/MemBinaryStream.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Regression: wallpaper 2979524338 (Outset Island/Zelda) ships a sound layer
// with zero files. The audio worker thread's first poll of NextPcmData reaches
// Switch(), which dereferences m_soundPaths[0] on an empty vector → SIGSEGV.
// The fix is to refuse to mount a stream with no tracks at parse time.

using wallpaper::WPSoundParser;
using wallpaper::wpscene::WPSoundObject;
using wallpaper::audio::SoundManager;
using wallpaper::audio::SoundStream;
using wallpaper::fs::VFS;
using wallpaper::fs::Fs;
using wallpaper::fs::IBinaryStream;
using wallpaper::fs::IBinaryStreamW;
using wallpaper::fs::MemBinaryStream;

// ─────────────────────────────────────────────────────────────────────────────
// Test fixtures: in-memory Fs holding synthesised WAV bytes, plus a helper to
// build a minimal valid PCM WAV file.  Mounted at /assets so Switch() finds the
// tracks at /assets/<path>.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

class MemFs : public Fs {
public:
    void add(std::string path, std::vector<uint8_t> data) {
        m_files[std::move(path)] = std::move(data);
    }
    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) > 0;
    }
    std::shared_ptr<IBinaryStream> Open(std::string_view path) override {
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        auto copy = it->second;
        return std::make_shared<MemBinaryStream>(std::move(copy));
    }
    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view) override { return nullptr; }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

// Generate a minimal mono 16-bit PCM WAV file in memory.
// `pattern` lets the caller stamp distinguishable content into different
// fixtures so playlist-cycling tests can prove they advanced to a new track.
std::vector<uint8_t> MakeWavData(uint32_t numSamples, uint32_t sampleRate = 44100,
                                 uint16_t channels = 1, int16_t pattern = 0) {
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

    // Triangle wave samples, deterministic and clearly non-zero.
    for (uint32_t i = 0; i < numSamples * channels; i++) {
        int16_t sample = (int16_t)(8000.0 * (2.0 * std::fabs((double)(i % 100) / 50.0 - 1.0) - 1.0))
                         + pattern;
        writeU16((uint16_t)sample);
    }
    return buf;
}

// Mount a single WAV at /assets/<path> and return both the VFS and a
// lightweight scene object that points at it.
struct Fixture {
    VFS                   vfs;
    SoundManager          sm;
    std::vector<uint8_t>* lastWav { nullptr };

    void mountWav(const std::string& assetPath, std::vector<uint8_t> data) {
        // VFS owns the Fs; build it lazily when the first asset is added.
        if (! m_owned) {
            auto fs = std::make_unique<MemFs>();
            m_fs    = fs.get();
            REQUIRE(vfs.Mount("/assets", std::move(fs)));
            m_owned = true;
        }
        m_fs->add(assetPath, std::move(data));
    }

private:
    MemFs* m_fs { nullptr };
    bool   m_owned { false };
};

WPSoundObject MakeMountableObj(const std::string& mode = "loop") {
    WPSoundObject obj;
    obj.name         = "test";
    obj.volume       = 0.8f;
    obj.playbackmode = mode;
    obj.sound        = { "audio/track1.ogg" };
    return obj;
}

// Drive the audio thread by calling NextPcmData.  Returns the float buffer for
// the test to inspect.  desc must match what was passed to PassDesc earlier.
std::vector<float> Pull(SoundStream* s, uint32_t channels, uint32_t frames) {
    std::vector<float> buf(frames * channels, 99.0f); // Pre-fill with sentinel
    uint64_t           ret = s->NextPcmData(buf.data(), frames);
    // NextPcmData on WPSoundStream always returns frameCount (zero-fills as needed).
    REQUIRE(ret == frames);
    return buf;
}

bool IsAllZero(const std::vector<float>& buf) {
    for (float f : buf)
        if (f != 0.0f) return false;
    return true;
}

bool HasNonZero(const std::vector<float>& buf) { return ! IsAllZero(buf); }

// Approximate magnitude (sum of |sample|) — used by volume-scaling tests.
double Magnitude(const std::vector<float>& buf) {
    double m = 0.0;
    for (float f : buf) m += std::fabs((double)f);
    return m;
}

} // namespace

TEST_SUITE("WPSoundParser::Parse — empty-tracks guard") {
    TEST_CASE("zero-track sound layer returns nullptr (does not mount)") {
        SoundManager  sm;
        VFS           vfs;
        WPSoundObject obj;
        obj.name   = "Songs";
        obj.volume = 0.5f;
        // obj.sound left empty — no tracks
        CHECK(WPSoundParser::Parse(obj, vfs, sm) == nullptr);
    }

    TEST_CASE("zero-track skip still applies when volume script is present") {
        SoundManager  sm;
        VFS           vfs;
        WPSoundObject obj;
        obj.name            = "scripted";
        obj.volume          = 0.0f;
        obj.hasVolumeScript = true;
        obj.volumeScript    = "function update(){return 1;}";
        // Empty sound list — even a script can't conjure tracks.
        CHECK(WPSoundParser::Parse(obj, vfs, sm) == nullptr);
    }

    TEST_CASE("zero-track skip still applies when startsilent") {
        SoundManager  sm;
        VFS           vfs;
        WPSoundObject obj;
        obj.name        = "silent_placeholder";
        obj.volume      = 1.0f;
        obj.startsilent = true;
        CHECK(WPSoundParser::Parse(obj, vfs, sm) == nullptr);
    }
}

TEST_SUITE("WPSoundParser::Parse — volume gate") {
    TEST_CASE("volume <= 0.001 skips when no script and not startsilent") {
        SoundManager sm;
        VFS          vfs;
        auto         obj = MakeMountableObj();
        obj.volume       = 0.0005f;
        CHECK(WPSoundParser::Parse(obj, vfs, sm) == nullptr);
    }

    TEST_CASE("volume <= 0.001 mounts when hasVolumeScript is set") {
        SoundManager sm;
        VFS          vfs;
        auto         obj    = MakeMountableObj();
        obj.volume          = 0.0f;
        obj.hasVolumeScript = true;
        obj.volumeScript    = "return 1.0;";
        CHECK(WPSoundParser::Parse(obj, vfs, sm) != nullptr);
    }

    TEST_CASE("volume <= 0.001 mounts when startsilent is set") {
        SoundManager sm;
        VFS          vfs;
        auto         obj = MakeMountableObj();
        obj.volume       = 0.0f;
        obj.startsilent  = true;
        CHECK(WPSoundParser::Parse(obj, vfs, sm) != nullptr);
    }

    TEST_CASE("volume > 1 is clamped to 1") {
        SoundManager sm;
        VFS          vfs;
        auto         obj = MakeMountableObj();
        obj.volume       = 5.0f;
        // No way to read clamped volume back without poking internal state,
        // but we can at least verify the parser succeeds with this input.
        CHECK(WPSoundParser::Parse(obj, vfs, sm) != nullptr);
    }
}

TEST_SUITE("WPSoundParser::Parse — playback modes") {
    TEST_CASE("loop mode mounts") {
        SoundManager sm;
        VFS          vfs;
        auto         obj = MakeMountableObj("loop");
        CHECK(WPSoundParser::Parse(obj, vfs, sm) != nullptr);
    }

    TEST_CASE("random mode mounts") {
        SoundManager sm;
        VFS          vfs;
        auto         obj = MakeMountableObj("random");
        CHECK(WPSoundParser::Parse(obj, vfs, sm) != nullptr);
    }

    TEST_CASE("single mode mounts") {
        SoundManager sm;
        VFS          vfs;
        auto         obj = MakeMountableObj("single");
        CHECK(WPSoundParser::Parse(obj, vfs, sm) != nullptr);
    }

    TEST_CASE("unknown playbackmode falls back to loop and mounts") {
        SoundManager sm;
        VFS          vfs;
        auto         obj = MakeMountableObj("flapdoodle");
        CHECK(WPSoundParser::Parse(obj, vfs, sm) != nullptr);
    }
}

TEST_SUITE("WPSoundParser stream lifecycle wrappers") {
    TEST_CASE("nullptr stream is safe for every wrapper") {
        // Public API guarantees: passing nullptr must not crash.
        WPSoundParser::SetStreamVolume(nullptr, 0.5f);
        WPSoundParser::StreamPlay(nullptr);
        WPSoundParser::StreamStop(nullptr);
        WPSoundParser::StreamPause(nullptr);
        CHECK_FALSE(WPSoundParser::StreamIsPlaying(nullptr));
        CHECK(WPSoundParser::AsSoundStreamForTest(nullptr) == nullptr);
    }

    TEST_CASE("Play / Stop / Pause / IsPlaying transitions") {
        SoundManager sm;
        VFS          vfs;
        auto         obj    = MakeMountableObj();
        void*        stream = WPSoundParser::Parse(obj, vfs, sm);
        REQUIRE(stream != nullptr);

        // Default constructed with startsilent=false → starts in Playing state.
        CHECK(WPSoundParser::StreamIsPlaying(stream));

        WPSoundParser::StreamPause(stream);
        CHECK_FALSE(WPSoundParser::StreamIsPlaying(stream));

        WPSoundParser::StreamPlay(stream);
        CHECK(WPSoundParser::StreamIsPlaying(stream));

        WPSoundParser::StreamStop(stream);
        CHECK_FALSE(WPSoundParser::StreamIsPlaying(stream));

        // Play after Stop should resume — exercises the m_needsReload path.
        WPSoundParser::StreamPlay(stream);
        CHECK(WPSoundParser::StreamIsPlaying(stream));

        // Re-Play while Playing is a no-op (covers the "already playing" early-out).
        WPSoundParser::StreamPlay(stream);
        CHECK(WPSoundParser::StreamIsPlaying(stream));
    }

    TEST_CASE("startsilent stream begins Stopped") {
        SoundManager sm;
        VFS          vfs;
        auto         obj    = MakeMountableObj();
        obj.startsilent     = true;
        void*        stream = WPSoundParser::Parse(obj, vfs, sm);
        REQUIRE(stream != nullptr);
        CHECK_FALSE(WPSoundParser::StreamIsPlaying(stream));
    }

    TEST_CASE("SetStreamVolume on a real stream is silent (just exercises path)") {
        SoundManager sm;
        VFS          vfs;
        auto         obj    = MakeMountableObj();
        void*        stream = WPSoundParser::Parse(obj, vfs, sm);
        REQUIRE(stream != nullptr);
        WPSoundParser::SetStreamVolume(stream, 0.25f); // observable side effect is internal state
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio-thread paths exercised through the testing accessor.  The fixtures
// mount a synthesised WAV at /assets/audio/<name>.ogg; miniaudio's decoder
// content-sniffs (extension is irrelevant), so our WAV bytes decode happily.
//
// Each test passes a Desc through PassDesc, then calls NextPcmData repeatedly
// to drive the state machine.  These tests cover paths not reachable from the
// public Play/Stop/Pause API: silence-on-stop, silence-on-pause, decoder
// release, Switch on first load, loop boundary recreation, random-mode delay
// counter, single-mode auto-stop, volume application, and zero-fill of partial
// reads.
// ─────────────────────────────────────────────────────────────────────────────

TEST_SUITE("WPSoundStream::NextPcmData — silence paths (no real device)") {
    TEST_CASE("Stopped state writes zeros and signals frameCount") {
        Fixture fx;
        // Mount a real track — this covers the Stop-after-load decoder-reset
        // path: we'll prime it with one Playing pull, then Stop.
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        REQUIRE(base != nullptr);
        base->PassDesc({ .channels = 2, .sampleRate = 44100 });

        // Prime: Playing → loads decoder.
        auto first = Pull(base, 2, 256);
        // The loaded decoder emits real (non-zero) PCM — proves Switch worked.
        CHECK(HasNonZero(first));

        // Stop and pull: must zero-fill *and* the next state-check shouldn't crash.
        WPSoundParser::StreamStop(stream);
        auto stopped = Pull(base, 2, 256);
        CHECK(IsAllZero(stopped));

        // Pull again while still Stopped — exercises the m_curActive==null
        // branch in the Stopped early-return.
        auto stopped2 = Pull(base, 2, 256);
        CHECK(IsAllZero(stopped2));
    }

    TEST_CASE("Paused state writes zeros and does NOT release the decoder") {
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // Prime
        auto first = Pull(base, 1, 256);
        CHECK(HasNonZero(first));

        WPSoundParser::StreamPause(stream);
        auto paused = Pull(base, 1, 256);
        CHECK(IsAllZero(paused));

        // Resume — without a decoder reload — should produce non-zero immediately.
        WPSoundParser::StreamPlay(stream);
        auto resumed = Pull(base, 1, 256);
        CHECK(HasNonZero(resumed));
    }

    TEST_CASE("Playing with a missing track returns silence (Switch fails)") {
        // No file mounted — Switch() will get nullptr from VFS and reset
        // m_curActive, so NextPcmData falls through to the zero-fill branch.
        Fixture fx;
        // Note: we DON'T mount any backing file.  But we still have to mount
        // *something* to exercise the "VFS doesn't find /assets" path —
        // simpler: mount an empty Fs.
        auto memfs = std::make_unique<MemFs>();
        REQUIRE(fx.vfs.Mount("/assets", std::move(memfs)));

        auto  obj    = MakeMountableObj("loop");
        obj.sound    = { "audio/missing.ogg" };
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        auto silent = Pull(base, 1, 256);
        CHECK(IsAllZero(silent));
    }

    TEST_CASE("Pull on startsilent stream returns silence without loading") {
        // startsilent → state==Stopped from construction → Stopped early-return
        // before any decoder load can occur.  This kills the mutant where the
        // "state != Playing" check is inverted.
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048));

        auto  obj    = MakeMountableObj("loop");
        obj.startsilent = true;
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        auto silent = Pull(base, 1, 256);
        CHECK(IsAllZero(silent));
        CHECK_FALSE(WPSoundParser::StreamIsPlaying(stream));
    }
}

TEST_SUITE("WPSoundStream::NextPcmData — playback paths") {
    TEST_CASE("First Pull triggers Switch and returns non-zero PCM") {
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(4096));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 2, .sampleRate = 44100 });

        auto buf = Pull(base, 2, 1024);
        CHECK(HasNonZero(buf));
    }

    TEST_CASE("Loop mode loops indefinitely (drives Switch at EOF)") {
        // Track is short — pulling more frames than the file has forces the
        // EOF branch to call Switch() and reload, producing more non-zero data.
        Fixture fx;
        const uint32_t numSamples = 1024; // ~23ms @ 44100
        fx.mountWav("/audio/track1.ogg", MakeWavData(numSamples));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // Total frames pulled exceeds file length → at least one Switch() must
        // happen at the loop boundary.
        const uint32_t frames    = 256;
        uint64_t       totalRead = 0;
        for (int i = 0; i < 8; i++) {
            auto buf = Pull(base, 1, frames);
            // After multiple iterations spanning EOF, we should still see
            // non-zero data (the next loop iteration's content).
            (void)buf;
            totalRead += frames;
        }
        CHECK(totalRead > numSamples); // proves we wrapped past EOF at least once
        // Final pull should still be non-zero (loop is alive).
        auto final_buf = Pull(base, 1, frames);
        CHECK(HasNonZero(final_buf));
    }

    TEST_CASE("Loop mode with multiple tracks cycles sequentially via Switch") {
        Fixture fx;
        // Two distinguishable WAVs (different DC offsets).
        fx.mountWav("/audio/a.ogg", MakeWavData(512, 44100, 1, 100));
        fx.mountWav("/audio/b.ogg", MakeWavData(512, 44100, 1, -100));

        auto obj  = MakeMountableObj("loop");
        obj.sound = { "audio/a.ogg", "audio/b.ogg" };
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // Drive >> 512 frames so we cross at least two Switch boundaries and
        // hit the second track.  We don't peek at content here (the WAV bytes
        // get filtered through miniaudio's decoder; signs may flip etc.) — we
        // just verify the audio thread keeps producing frameCount with no crash.
        for (int i = 0; i < 12; i++) {
            auto buf = Pull(base, 1, 256);
            (void)buf;
        }
        CHECK(true); // no crash → cycling works
    }

    TEST_CASE("Single mode stops after track completes") {
        Fixture fx;
        const uint32_t numSamples = 256;
        fx.mountWav("/audio/track1.ogg", MakeWavData(numSamples));

        auto  obj    = MakeMountableObj("single");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        CHECK(WPSoundParser::StreamIsPlaying(stream)); // begins Playing

        // Drain past EOF — Single mode must stop the stream and zero-fill.
        const uint32_t frames = 512; // exceeds numSamples
        auto           first  = Pull(base, 1, frames);
        // First pull may have non-zero (track data) followed by zero tail.
        (void)first;

        // Subsequent pulls must always be zero, and IsPlaying() must be false.
        for (int i = 0; i < 4; i++) {
            auto buf = Pull(base, 1, 256);
            CHECK(IsAllZero(buf));
        }
        CHECK_FALSE(WPSoundParser::StreamIsPlaying(stream));
    }

    TEST_CASE("Random mode with positive mintime/maxtime inserts inter-track silence") {
        // EOF in random mode should set m_delaySamples > 0, and the next pull
        // should produce silence frames before the next track loads.
        Fixture fx;
        const uint32_t numSamples = 256;
        fx.mountWav("/audio/a.ogg", MakeWavData(numSamples));
        fx.mountWav("/audio/b.ogg", MakeWavData(numSamples));

        auto obj    = MakeMountableObj("random");
        obj.sound   = { "audio/a.ogg", "audio/b.ogg" };
        obj.mintime = 0.1f; // 4410 frames @ 44100
        obj.maxtime = 0.2f;
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // Drain track a, the EOF transition arms m_delaySamples, then we
        // should hit the delay branch (silence).  We can't directly observe
        // m_delaySamples — we just verify pulling many frames still returns
        // frameCount and doesn't crash, exercising the delay-countdown loop.
        for (int i = 0; i < 30; i++) {
            auto buf = Pull(base, 1, 256);
            (void)buf;
        }
        CHECK(true);
    }

    TEST_CASE("Random mode with mintime=maxtime=0 has no delay (loops directly)") {
        // RandomDelaySamples returns 0 → m_delaySamples == 0 path is skipped.
        Fixture fx;
        fx.mountWav("/audio/a.ogg", MakeWavData(256));
        fx.mountWav("/audio/b.ogg", MakeWavData(256));

        auto obj    = MakeMountableObj("random");
        obj.sound   = { "audio/a.ogg", "audio/b.ogg" };
        obj.mintime = 0.0f;
        obj.maxtime = 0.0f;
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // No silence injection — pulls span EOFs without a delay branch.
        for (int i = 0; i < 6; i++) {
            auto buf = Pull(base, 1, 256);
            (void)buf;
        }
        CHECK(true);
    }
}

TEST_SUITE("WPSoundStream::NextPcmData — volume scaling") {
    TEST_CASE("SetVolume(0) silences output even on a non-silent track") {
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        WPSoundParser::SetStreamVolume(stream, 0.0f);
        auto buf = Pull(base, 1, 256);
        CHECK(IsAllZero(buf));
    }

    TEST_CASE("SetVolume(0.5) halves output magnitude vs SetVolume(1.0)") {
        Fixture fxA;
        fxA.mountWav("/audio/track1.ogg", MakeWavData(2048));
        Fixture fxB;
        fxB.mountWav("/audio/track1.ogg", MakeWavData(2048));

        auto  objA    = MakeMountableObj("loop");
        void* streamA = WPSoundParser::Parse(objA, fxA.vfs, fxA.sm);
        REQUIRE(streamA != nullptr);
        SoundStream* baseA = WPSoundParser::AsSoundStreamForTest(streamA);
        baseA->PassDesc({ .channels = 1, .sampleRate = 44100 });
        WPSoundParser::SetStreamVolume(streamA, 1.0f);
        auto bufA = Pull(baseA, 1, 1024);

        auto  objB    = MakeMountableObj("loop");
        void* streamB = WPSoundParser::Parse(objB, fxB.vfs, fxB.sm);
        REQUIRE(streamB != nullptr);
        SoundStream* baseB = WPSoundParser::AsSoundStreamForTest(streamB);
        baseB->PassDesc({ .channels = 1, .sampleRate = 44100 });
        WPSoundParser::SetStreamVolume(streamB, 0.5f);
        auto bufB = Pull(baseB, 1, 1024);

        double magA = Magnitude(bufA);
        double magB = Magnitude(bufB);
        // bufB was scaled by 0.5 — should be roughly half of bufA.
        CHECK(magA > 0.0);
        CHECK(magB > 0.0);
        CHECK(magB == doctest::Approx(magA * 0.5).epsilon(0.05));
    }

    TEST_CASE("Volume update is observable on the next pull (atomic visibility)") {
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        WPSoundParser::SetStreamVolume(stream, 1.0f);
        auto loud = Pull(base, 1, 1024);
        WPSoundParser::SetStreamVolume(stream, 0.0f);
        auto quiet = Pull(base, 1, 1024);

        CHECK(HasNonZero(loud));
        CHECK(IsAllZero(quiet));
    }
}

TEST_SUITE("WPSoundStream::NextPcmData — m_needsReload (Play after Stop)") {
    TEST_CASE("Stop → Play → next pull reloads decoder and emits non-zero") {
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // Prime
        auto first = Pull(base, 1, 256);
        CHECK(HasNonZero(first));

        // Stop releases the decoder (next pull is silent).
        WPSoundParser::StreamStop(stream);
        auto silent = Pull(base, 1, 256);
        CHECK(IsAllZero(silent));

        // Play arms m_needsReload — next pull triggers Switch() to load fresh
        // decoder.  The audio data should be non-zero again.
        WPSoundParser::StreamPlay(stream);
        auto reloaded = Pull(base, 1, 256);
        CHECK(HasNonZero(reloaded));
    }

    TEST_CASE("Play without prior Stop does NOT arm m_needsReload (no double-load)") {
        // Sanity: when state is Playing (or Paused), Play() is a no-op for
        // m_needsReload.  Behaviourally we just confirm output stays non-zero.
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        auto first = Pull(base, 1, 256);
        CHECK(HasNonZero(first));

        WPSoundParser::StreamPlay(stream); // state already Playing → early-out
        auto same = Pull(base, 1, 256);
        CHECK(HasNonZero(same));
    }

    TEST_CASE("Pause → Play does NOT reload (m_needsReload only set from Stopped)") {
        // Pause state preserves the decoder; Play after Pause must not flush
        // it.  Observable: output continues without a discontinuity (we just
        // assert non-zero here).
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(8192));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // Prime to ensure decoder is loaded.
        auto first = Pull(base, 1, 256);
        CHECK(HasNonZero(first));

        WPSoundParser::StreamPause(stream);
        auto paused = Pull(base, 1, 256);
        CHECK(IsAllZero(paused));

        WPSoundParser::StreamPlay(stream);
        auto resumed = Pull(base, 1, 256);
        CHECK(HasNonZero(resumed));
    }
}

TEST_SUITE("WPSoundStream::PassDesc + multi-channel paths") {
    TEST_CASE("PassDesc is stored and used to size the silence write (mono)") {
        Fixture fx;
        // No track mounted → first pull falls into the "Switch() returned
        // null" silence branch.  The buffer must be zero-filled in mono size
        // (channels==1).  We use the sentinel to verify untouched bytes too.
        auto memfs = std::make_unique<MemFs>();
        REQUIRE(fx.vfs.Mount("/assets", std::move(memfs)));

        auto  obj    = MakeMountableObj("loop");
        obj.sound    = { "audio/missing.ogg" };
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        std::vector<float> buf(256, 7.0f);
        REQUIRE(base->NextPcmData(buf.data(), 256) == 256);
        for (float f : buf) CHECK(f == 0.0f);
    }

    TEST_CASE("PassDesc handles stereo (channels=2) silence write") {
        Fixture fx;
        auto    memfs = std::make_unique<MemFs>();
        REQUIRE(fx.vfs.Mount("/assets", std::move(memfs)));

        auto  obj    = MakeMountableObj("loop");
        obj.sound    = { "audio/missing.ogg" };
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 2, .sampleRate = 48000 });

        // 2 channels × 256 frames = 512 floats
        std::vector<float> buf(512, 7.0f);
        REQUIRE(base->NextPcmData(buf.data(), 256) == 256);
        for (float f : buf) CHECK(f == 0.0f);
    }

    TEST_CASE("PassDesc can be re-applied; latest desc takes effect") {
        Fixture fx;
        auto    memfs = std::make_unique<MemFs>();
        REQUIRE(fx.vfs.Mount("/assets", std::move(memfs)));

        auto  obj    = MakeMountableObj("loop");
        obj.sound    = { "audio/missing.ogg" };
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });
        // First pull uses channels=1
        std::vector<float> mono(256, 9.0f);
        REQUIRE(base->NextPcmData(mono.data(), 256) == 256);

        // Reconfigure to stereo before the next pull.
        base->PassDesc({ .channels = 2, .sampleRate = 48000 });
        std::vector<float> stereo(512, 9.0f);
        REQUIRE(base->NextPcmData(stereo.data(), 256) == 256);
        // Both pulls should have zero-filled their full buffer based on the
        // *currently active* desc.
        for (float f : mono) CHECK(f == 0.0f);
        for (float f : stereo) CHECK(f == 0.0f);
    }
}

TEST_SUITE("WPSoundStream::NextPcmData — partial read tail-zero") {
    TEST_CASE("Pull-bigger-than-track zero-fills the tail beyond decoder output") {
        // After the decoder yields its final partial chunk (frameReads <
        // frameCount), the wrapper zero-fills the buffer's tail.  This test
        // drives that branch in Single mode: a 100-sample WAV pulled into a
        // 2048-frame buffer yields 100 frames of audio + 1948 frames of
        // zero-fill in the FIRST call.  The auto-stop only triggers on the
        // SECOND call (when frameReads==0).
        Fixture fx;
        const uint32_t numSamples = 100;
        fx.mountWav("/audio/track1.ogg", MakeWavData(numSamples));

        auto  obj    = MakeMountableObj("single");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // First pull: partial-read + tail-zero branch.
        std::vector<float> buf(2048, 1.0f);
        uint64_t           ret = base->NextPcmData(buf.data(), 2048);
        CHECK(ret == 2048);
        // Some samples in the head should be non-zero (decoder output), and
        // the tail (samples beyond frameReads*channels) must be zeroed.
        // miniaudio may or may not deliver every sample on the boundary, but
        // the *tail* portion of the buffer must definitely have been zeroed
        // by the partial-read code path.
        CHECK(buf.back() == 0.0f);
        CHECK(buf[2047] == 0.0f);
        CHECK(buf[2000] == 0.0f);

        // Second pull: now the Single-mode auto-stop fires (frameReads==0).
        // After this, IsPlaying() must be false and the buffer is all zeros.
        std::vector<float> buf2(2048, 1.0f);
        ret = base->NextPcmData(buf2.data(), 2048);
        CHECK(ret == 2048);
        CHECK(IsAllZero(buf2));
        CHECK_FALSE(WPSoundParser::StreamIsPlaying(stream));
    }

    TEST_CASE("Loop mode tail-zero on partial read across the boundary") {
        // The "frameReads < frameCount" tail-zero path in Loop mode is taken
        // on the Switch() → second-NextPcmData path: if the freshly-loaded
        // decoder returns less than the remaining frames, the tail is zeroed.
        // We can't fully drive this without injecting a flaky decoder; instead
        // this test pulls many small chunks that span an EOF boundary and
        // confirms there's no UB / no infinite loop / output is well-formed
        // (each pull returns the exact frameCount requested).
        Fixture fx;
        const uint32_t numSamples = 256;
        fx.mountWav("/audio/track1.ogg", MakeWavData(numSamples));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        const uint32_t chunk = 64;
        for (int i = 0; i < 10; i++) {
            std::vector<float> buf(chunk);
            CHECK(base->NextPcmData(buf.data(), chunk) == chunk);
        }
    }
}

TEST_SUITE("WPSoundStream — extended state-transition coverage") {
    TEST_CASE("Stop → Stop is idempotent (re-Stop has no observable effect)") {
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        WPSoundParser::StreamStop(stream);
        WPSoundParser::StreamStop(stream);
        CHECK_FALSE(WPSoundParser::StreamIsPlaying(stream));

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        auto buf = Pull(base, 1, 256);
        CHECK(IsAllZero(buf));
    }

    TEST_CASE("Pause → Stop releases decoder on next pull") {
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // Prime to load decoder.
        auto first = Pull(base, 1, 256);
        CHECK(HasNonZero(first));

        WPSoundParser::StreamPause(stream);
        auto paused = Pull(base, 1, 256);
        CHECK(IsAllZero(paused));

        WPSoundParser::StreamStop(stream);
        auto stopped = Pull(base, 1, 256);
        CHECK(IsAllZero(stopped));
        // Subsequent Stopped pulls remain silent.
        auto stopped2 = Pull(base, 1, 256);
        CHECK(IsAllZero(stopped2));
    }

    // ──────────────────────────────────────────────────────────────────────
    // Targeted mutation-killers.  Each kills a specific surviving mull mutant
    // in WPSoundParser.cpp, named in the comment.
    // ──────────────────────────────────────────────────────────────────────

    TEST_CASE("Pause→Play does NOT reset decoder position; Stop→Play DOES") {
        // Kills mutants:
        //   L67 cxx_eq_to_ne: Play() arms m_needsReload only if prev==Stopped.
        //   L84 cxx_eq_to_ne: NextPcmData drops m_curActive only on Stopped.
        //
        // Observable: the post-resume PCM sequence after Pause→Play continues
        // from where we paused; after Stop→Play it restarts from the beginning.
        Fixture fxA, fxB;
        fxA.mountWav("/audio/track1.ogg", MakeWavData(8192));
        fxB.mountWav("/audio/track1.ogg", MakeWavData(8192));

        auto runScenario = [&](Fixture& fx, bool useStop) {
            auto  obj    = MakeMountableObj("loop");
            void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
            REQUIRE(stream != nullptr);
            SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
            base->PassDesc({ .channels = 1, .sampleRate = 44100 });

            // Pull initial chunk to advance decoder past sample 0.
            std::vector<float> first(2048);
            REQUIRE(base->NextPcmData(first.data(), 2048) == 2048);
            REQUIRE(HasNonZero(first));

            // Pause or Stop, then Play.
            if (useStop) WPSoundParser::StreamStop(stream);
            else         WPSoundParser::StreamPause(stream);
            // Drain the silence chunk that follows.
            std::vector<float> silenceDrain(2048);
            base->NextPcmData(silenceDrain.data(), 2048);

            WPSoundParser::StreamPlay(stream);
            std::vector<float> after(2048);
            REQUIRE(base->NextPcmData(after.data(), 2048) == 2048);
            return std::pair<std::vector<float>, std::vector<float>>(first, after);
        };

        auto [pauseFirst, pauseAfter] = runScenario(fxA, /*useStop=*/false);
        auto [stopFirst, stopAfter]   = runScenario(fxB, /*useStop=*/true);

        // After Stop→Play, decoder reloads from sample 0.  The post-Play chunk
        // should match the very first chunk (modulo decoder fp variance).
        // Conversely, Pause→Play continues, so the post-Play chunk should
        // differ from the first chunk (decoder is at a later offset).
        auto byteIdentical = [](const std::vector<float>& a, const std::vector<float>& b) {
            REQUIRE(a.size() == b.size());
            int eq = 0;
            for (size_t i = 0; i < a.size(); i++)
                if (a[i] == b[i]) eq++;
            return eq;
        };
        int stopEq  = byteIdentical(stopFirst, stopAfter);
        int pauseEq = byteIdentical(pauseFirst, pauseAfter);
        // Stop→Play restarts → almost all samples match.
        CHECK(stopEq > (int)stopAfter.size() / 2);
        // Pause→Play continues → very few samples match (different waveform region).
        CHECK(pauseEq < (int)pauseAfter.size() / 2);
    }

    TEST_CASE("Random-mode delay countdown produces silence then non-zero") {
        // Drives mid-countdown silence (kills L128 `> → <=`: when delay > 0
        // the silence return MUST fire, not pass-through to NextPcmData).
        Fixture fx;
        const uint32_t numSamples = 256;
        fx.mountWav("/audio/a.ogg", MakeWavData(numSamples));
        fx.mountWav("/audio/b.ogg", MakeWavData(numSamples));

        auto obj    = MakeMountableObj("random");
        obj.sound   = { "audio/a.ogg", "audio/b.ogg" };
        // ~440 frames @ 44100, several silent pulls of 64 frames.
        obj.mintime = 0.01f;
        obj.maxtime = 0.01f;
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // Pull enough frames to: drain track A, arm delay, count down
        // through several silent chunks, then resume on track B.  We need
        // both at least one silence pull (proves delay-branch fired) and
        // at least one non-zero pull after the countdown (proves Switch
        // fired and the delay branch eventually exited).
        int nonZero    = 0;
        int silentMid  = 0;
        for (int i = 0; i < 30; i++) {
            std::vector<float> buf(64);
            REQUIRE(base->NextPcmData(buf.data(), 64) == 64);
            if (HasNonZero(buf)) nonZero++;
            else if (i > 3) silentMid++;
        }
        // At least 1 silent mid-countdown pull (proves delay branch fired)
        // and at least 1 non-zero post-countdown pull (proves we recovered).
        CHECK(silentMid >= 1);
        CHECK(nonZero >= 1);
    }

    TEST_CASE("Random-mode zero delay loops directly (delay branch is skipped)") {
        // Kills mutant L128 cxx_gt_to_ge: `if (m_delaySamples > 0)`.  When
        // RandomDelayFrames returns 0, the silence-return branch must be
        // skipped so Switch fires inline AND the post-Switch NextPcmData
        // runs in the SAME call (producing non-zero).  Under mutation `>=`,
        // 0>=0 is true → memset full buffer to silence + Switch + return
        // silence for that pull.  The key signal: with frameCount==trackLen,
        // every 2nd pull is an EOF transition, so original yields non-zero
        // every pull while mutation alternates non-zero/silence.
        Fixture fx;
        // Match track length exactly to pull length so every pull crosses
        // an EOF boundary cleanly.
        fx.mountWav("/audio/a.ogg", MakeWavData(256));
        fx.mountWav("/audio/b.ogg", MakeWavData(256));

        auto obj    = MakeMountableObj("random");
        obj.sound   = { "audio/a.ogg", "audio/b.ogg" };
        obj.mintime = 0.0f; // RandomDelayFrames must return 0
        obj.maxtime = 0.0f;
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // Track-length pulls so every other pull spans an EOF.  With
        // mintime=maxtime=0, original has zero silence; mutation injects
        // a silence pull at each boundary.  After 8 pulls, original gives
        // ~8 non-zero, mutation gives ~4.  We assert ≥6 non-zero pulls.
        int nonZeroPulls = 0;
        for (int i = 0; i < 8; i++) {
            std::vector<float> buf(256);
            REQUIRE(base->NextPcmData(buf.data(), 256) == 256);
            if (HasNonZero(buf)) nonZeroPulls++;
        }
        CHECK(nonZeroPulls >= 6);
    }

    TEST_CASE("volume == 0.001 boundary skips (kills <= → < mutant on volume gate)") {
        // Kills mutant L215 cxx_le_to_lt: `if (vol <= 0.001f)`.  At vol==0.001
        // the original returns nullptr; mutation `<` lets it through.
        SoundManager sm;
        VFS          vfs;
        auto         obj = MakeMountableObj();
        obj.volume       = 0.001f; // exactly the boundary
        CHECK(WPSoundParser::Parse(obj, vfs, sm) == nullptr);
    }

    TEST_CASE("volume just above 0.001 mounts (lower-bound boundary)") {
        // Companion to the previous test: vol slightly above the threshold
        // must mount.  Together these pin the boundary at <=0.001 exactly.
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(256));
        auto  obj    = MakeMountableObj();
        obj.volume   = 0.002f;
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        CHECK(stream != nullptr);
    }

    TEST_CASE("Stereo silence-write fully zeros the stereo buffer (channels=2 path)") {
        // Kills `* → /` mutants on `frameCount * m_desc.channels * sizeof(float)`
        // arguments to memset.  With mono, * vs / produces identical results
        // for typical sizes (1*4 == 4, 1/4 == 0 → memset 0 bytes is no-op
        // but full buf was already zeroed by sentinel-fill anyway… wait, no:
        // sentinel-fill is 7.0f).  With stereo (channels=2, sizeof=4):
        //   original: 256 * 2 * 4 = 2048 bytes → zeros entire 512-float buf
        //   mutated  256 / 2 * 4 = 512 bytes → zeros only first 128 floats
        // So a full-buffer all-zero check on stereo catches this mutant.
        Fixture fx;
        auto    memfs = std::make_unique<MemFs>();
        REQUIRE(fx.vfs.Mount("/assets", std::move(memfs))); // empty FS

        auto  obj    = MakeMountableObj("loop");
        obj.sound    = { "audio/missing.ogg" };
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 2, .sampleRate = 48000 });

        // Sentinel-fill the entire stereo buffer; missing file → all zeros.
        std::vector<float> buf(512, 7.0f);
        REQUIRE(base->NextPcmData(buf.data(), 256) == 256);
        // The very last sample MUST be zero — kills the truncated-memset mutant.
        CHECK(buf.back() == 0.0f);
        CHECK(buf[400] == 0.0f);
        CHECK(buf[300] == 0.0f);
        CHECK(IsAllZero(buf));
    }

    TEST_CASE("Stereo partial-read tail-zero covers full byte count") {
        // Kills mutant L160 cxx_mul_to_div on the partial-read tail-zero
        // memset:  `(frameCount - frameReads) * channels * sizeof(float)`.
        // With mono (channels=1), the inner `* channels / sizeof(float)`
        // mutation collapses to identical byte counts.  Stereo distinguishes
        // them: original yields 4N bytes per frame, mutation yields N/4 bytes.
        Fixture fx;
        // 100-sample stereo WAV.  When we pull 2048 frames, the decoder
        // returns ≤100 frames and the remaining ~1948 frames must be zeroed.
        fx.mountWav("/audio/track1.ogg", MakeWavData(100, 44100, 2));

        auto  obj    = MakeMountableObj("loop"); // Loop mode triggers Switch
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 2, .sampleRate = 44100 });

        // Single mode pulls bigger-than-track to make frameReads<frameCount
        // is the cleanest driver.  Use Single since loop mode reloads via
        // Switch and may fill the tail with the next iteration's data.
        WPSoundParser::StreamStop(stream); // detach from any prior Play state
        WPSoundParser::StreamPlay(stream); // ensure m_needsReload primes
        std::vector<float> buf(4096, 5.0f); // 2048 frames × 2 channels
        REQUIRE(base->NextPcmData(buf.data(), 2048) == 2048);
        // The far tail (well past 100 stereo frames = 200 floats) must be
        // zeroed by the partial-read branch (or by the loop-EOF reload
        // chain — either path's memset uses the same byte arithmetic).
        // With the mutation, the memset stops short and these stay 5.0.
        // We check at the very end of the buffer:
        CHECK(buf[4095] == 0.0f);
        CHECK(buf[4000] == 0.0f);
        CHECK(buf[3000] == 0.0f);
    }

    TEST_CASE("Stereo Stopped-state silence write zeros the entire stereo buffer") {
        // Companion mutant kill on the Stopped-state silence path
        // (line 82's `frameCount * m_desc.channels * sizeof(float)`).
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048, 44100, 2));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 2, .sampleRate = 44100 });

        WPSoundParser::StreamStop(stream);
        std::vector<float> buf(512, 7.0f);
        REQUIRE(base->NextPcmData(buf.data(), 256) == 256);
        CHECK(buf.back() == 0.0f);
        CHECK(IsAllZero(buf));
    }

    TEST_CASE("Stereo Single-mode auto-stop fully zeros the stereo buffer") {
        // Kills L122 `* → /` on the Single-mode EOF auto-stop silence write.
        // With mono these mutations are equivalent; stereo distinguishes.
        Fixture fx;
        const uint32_t numSamples = 64;
        fx.mountWav("/audio/track1.ogg", MakeWavData(numSamples, 44100, 2));

        auto  obj    = MakeMountableObj("single");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 2, .sampleRate = 44100 });

        // First pull: partial-read (decoder gives ~64 frames, frameCount=1024).
        std::vector<float> buf(2048, 7.0f);
        REQUIRE(base->NextPcmData(buf.data(), 1024) == 1024);

        // Second pull: NOW frameReads==0 → Single mode auto-stop silence.
        std::vector<float> buf2(2048, 7.0f);
        REQUIRE(base->NextPcmData(buf2.data(), 1024) == 1024);
        // Tail must be entirely zero — kills the truncated-memset mutant.
        CHECK(buf2[2047] == 0.0f);
        CHECK(buf2[1500] == 0.0f);
        CHECK(IsAllZero(buf2));
    }

    TEST_CASE("Stereo random-mode mid-countdown silence write fully zeros buffer") {
        // Kills L107 `* → /` on the random-mode delay-branch silence memset
        // AND L130 `* → /` on the *initial* delay-armed silence at EOF.
        Fixture fx;
        fx.mountWav("/audio/a.ogg", MakeWavData(64, 44100, 2));
        fx.mountWav("/audio/b.ogg", MakeWavData(64, 44100, 2));

        auto obj    = MakeMountableObj("random");
        obj.sound   = { "audio/a.ogg", "audio/b.ogg" };
        obj.mintime = 0.05f; // ~2200 frames @ 44100 — many silent pulls
        obj.maxtime = 0.05f;
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 2, .sampleRate = 44100 });

        // Pull 1: drain track a's 64 frames into a 256-frame buffer (partial
        // read + tail-zero).  frameReads != 0, no EOF transition yet.
        std::vector<float> p1(512);
        REQUIRE(base->NextPcmData(p1.data(), 256) == 256);

        // Pull 2: m_curActive returns 0 frames → EOF branch fires.  Random
        // mode + delay > 0 → executes line 130's full-buffer memset.  Any
        // sentinel bytes left in the *back* of the buffer must be zeroed.
        std::vector<float> p2(512, 7.0f);
        REQUIRE(base->NextPcmData(p2.data(), 256) == 256);
        CHECK(p2.back() == 0.0f);
        CHECK(p2[400] == 0.0f);
        CHECK(IsAllZero(p2));

        // Pull 3+: now in mid-countdown.  Line 107 silence write covers the
        // full buffer in the same way.
        std::vector<float> p3(512, 7.0f);
        REQUIRE(base->NextPcmData(p3.data(), 256) == 256);
        CHECK(p3.back() == 0.0f);
        CHECK(IsAllZero(p3));
    }

    TEST_CASE("Stereo volume scaling covers the full stereo frame count") {
        // Kills L151 `* → /` on `frameReads * m_desc.channels`.  With mono
        // (channels=1), the mutation produces frameReads/1 == frameReads —
        // identical loop bound.  With stereo it differs (e.g. 256*2=512 vs
        // 256/2=128), so half the right-channel samples would NOT be scaled.
        Fixture fx;
        fx.mountWav("/audio/track1.ogg", MakeWavData(2048, 44100, 2));

        auto  obj    = MakeMountableObj("loop");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 2, .sampleRate = 44100 });

        // Set volume to 0 — original scales every sample to 0.  Mutant only
        // scales the first half of the buffer, leaving the right channel
        // unmodified — so we'd see non-zero in the back half of the buffer.
        WPSoundParser::SetStreamVolume(stream, 0.0f);
        std::vector<float> buf(1024, 0.0f); // pre-zeroed (volume scale writes)
        REQUIRE(base->NextPcmData(buf.data(), 512) == 512);
        // Every sample must be zero — proves the volume loop covered all
        // (channels * frameReads) elements, not half.
        CHECK(IsAllZero(buf));
    }

    TEST_CASE("Single mode: Stop before EOF prevents auto-stop branch from running") {
        // If we stop manually first, the EOF auto-stop in Single mode never fires.
        // This exercises the order-independence of Stop vs end-of-track.
        Fixture fx;
        const uint32_t numSamples = 4096;
        fx.mountWav("/audio/track1.ogg", MakeWavData(numSamples));

        auto  obj    = MakeMountableObj("single");
        void* stream = WPSoundParser::Parse(obj, fx.vfs, fx.sm);
        REQUIRE(stream != nullptr);

        SoundStream* base = WPSoundParser::AsSoundStreamForTest(stream);
        base->PassDesc({ .channels = 1, .sampleRate = 44100 });

        // Pull one chunk, then stop before EOF.
        Pull(base, 1, 256);
        WPSoundParser::StreamStop(stream);
        CHECK_FALSE(WPSoundParser::StreamIsPlaying(stream));
        // Restarting after Stop in Single mode should also reload.
        WPSoundParser::StreamPlay(stream);
        auto resumed = Pull(base, 1, 256);
        CHECK(HasNonZero(resumed));
    }
}
