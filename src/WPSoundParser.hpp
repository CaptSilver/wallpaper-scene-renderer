#pragma once

namespace wallpaper
{

namespace audio
{
class SoundManager;
class SoundStream;
}
namespace fs
{
class VFS;
}
namespace wpscene
{
struct WPSoundObject;
}
class WPSoundStream; // forward declare
class WPSoundParser {
public:
    // Returns raw pointer to the mounted stream (for volume script updates), or nullptr if skipped
    static WPSoundStream* Parse(const wpscene::WPSoundObject&, fs::VFS&, audio::SoundManager&);
    // Thread-safe volume update via type-erased pointer (streamPtr from Scene::SoundVolumeScript)
    static void SetStreamVolume(void* stream, float volume);
    // Thread-safe sound layer control methods (type-erased WPSoundStream*)
    static void StreamPlay(void* stream);
    static void StreamStop(void* stream);
    static void StreamPause(void* stream);
    static bool StreamIsPlaying(void* stream);

    // Internal: testing accessor exposing the audio-thread base interface.
    // Tests use this to drive NextPcmData / PassDesc directly without bringing
    // up a real audio device.  Production callers must not use this.
    static audio::SoundStream* AsSoundStreamForTest(void* stream);
};
} // namespace wallpaper