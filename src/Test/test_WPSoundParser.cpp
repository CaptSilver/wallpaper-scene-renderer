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
