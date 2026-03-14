#pragma once

namespace wallpaper
{

namespace audio { class SoundManager; }
namespace fs { class VFS; }
namespace wpscene { class WPSoundObject; }
class WPSoundStream;  // forward declare
class WPSoundParser {
public:
	// Returns raw pointer to the mounted stream (for volume script updates), or nullptr if skipped
	static WPSoundStream* Parse(const wpscene::WPSoundObject&, fs::VFS&, audio::SoundManager&);
	// Thread-safe volume update via type-erased pointer (streamPtr from Scene::SoundVolumeScript)
	static void SetStreamVolume(void* stream, float volume);
};
}