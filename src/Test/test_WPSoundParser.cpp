#include <doctest.h>
#include "WPSoundParser.hpp"
#include "wpscene/WPSoundObject.h"
#include "Audio/SoundManager.h"
#include "Fs/VFS.h"

// Regression: wallpaper 2979524338 (Outset Island/Zelda) ships a sound layer
// with zero files. The audio worker thread's first poll of NextPcmData reaches
// Switch(), which dereferences m_soundPaths[0] on an empty vector → SIGSEGV.
// The fix is to refuse to mount a stream with no tracks at parse time.

using wallpaper::WPSoundParser;
using wallpaper::wpscene::WPSoundObject;
using wallpaper::audio::SoundManager;
using wallpaper::fs::VFS;

TEST_SUITE("WPSoundParser::Parse — empty-tracks guard") {
    TEST_CASE("zero-track sound layer returns nullptr (does not mount)") {
        SoundManager sm;
        VFS          vfs;
        WPSoundObject obj;
        obj.name   = "Songs";
        obj.volume = 0.5f;
        // obj.sound left empty — no tracks
        CHECK(WPSoundParser::Parse(obj, vfs, sm) == nullptr);
    }

    TEST_CASE("zero-track skip still applies when volume script is present") {
        SoundManager sm;
        VFS          vfs;
        WPSoundObject obj;
        obj.name            = "scripted";
        obj.volume          = 0.0f;
        obj.hasVolumeScript = true;
        obj.volumeScript    = "function update(){return 1;}";
        // Empty sound list — even a script can't conjure tracks.
        CHECK(WPSoundParser::Parse(obj, vfs, sm) == nullptr);
    }

    TEST_CASE("zero-track skip still applies when startsilent") {
        SoundManager sm;
        VFS          vfs;
        WPSoundObject obj;
        obj.name        = "silent_placeholder";
        obj.volume      = 1.0f;
        obj.startsilent = true;
        CHECK(WPSoundParser::Parse(obj, vfs, sm) == nullptr);
    }
}

// Minimal config that produces a mounted stream.  Only one path is needed
// for Parse to take the success branch — the audio thread is not started by
// MountStream, so no track data needs to exist on disk.
namespace
{
WPSoundObject MakeMountableObj(const std::string& mode = "loop") {
    WPSoundObject obj;
    obj.name         = "test";
    obj.volume       = 0.8f;
    obj.playbackmode = mode;
    obj.sound        = { "audio/track1.ogg" };
    return obj;
}
} // namespace

TEST_SUITE("WPSoundParser::Parse — volume gate") {
    TEST_CASE("volume <= 0.001 skips when no script and not startsilent") {
        SoundManager sm;
        VFS          vfs;
        auto         obj  = MakeMountableObj();
        obj.volume        = 0.0005f;
        CHECK(WPSoundParser::Parse(obj, vfs, sm) == nullptr);
    }

    TEST_CASE("volume <= 0.001 mounts when hasVolumeScript is set") {
        SoundManager sm;
        VFS          vfs;
        auto         obj      = MakeMountableObj();
        obj.volume            = 0.0f;
        obj.hasVolumeScript   = true;
        obj.volumeScript      = "return 1.0;";
        CHECK(WPSoundParser::Parse(obj, vfs, sm) != nullptr);
    }

    TEST_CASE("volume <= 0.001 mounts when startsilent is set") {
        SoundManager sm;
        VFS          vfs;
        auto         obj    = MakeMountableObj();
        obj.volume          = 0.0f;
        obj.startsilent     = true;
        CHECK(WPSoundParser::Parse(obj, vfs, sm) != nullptr);
    }

    TEST_CASE("volume > 1 is clamped to 1") {
        SoundManager sm;
        VFS          vfs;
        auto         obj  = MakeMountableObj();
        obj.volume        = 5.0f;
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
