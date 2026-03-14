#include "WPSoundParser.hpp"
#include "Audio/SoundManager.h"
#include "Fs/VFS.h"
#include "wpscene/WPSoundObject.h"
#include "Utils/Logging.h"

#include <string>
#include <string_view>
#include <atomic>

namespace wallpaper
{

enum class PlaybackMode
{
    Random,
    Loop
};

static PlaybackMode ToPlaybackMode(std::string_view s) {
    if (s == "loop")
        return PlaybackMode::Loop;
    else if (s == "random")
        return PlaybackMode::Random;
    return PlaybackMode::Loop;
};

class WPSoundStream : public audio::SoundStream {
public:
    struct Config {
        float        maxtime { 10.0f };
        float        mintime { 0.0f };
        float        volume { 1.0f };
        PlaybackMode mode { PlaybackMode::Loop };
    };
    WPSoundStream(const std::vector<std::string>& paths, fs::VFS& vfs, Config c)
        : vfs(vfs), m_config(c), m_soundPaths(paths),
          m_runtimeVolume(c.volume) {};
    virtual ~WPSoundStream() = default;

    // Thread-safe volume update (called from QML thread via script evaluation)
    void SetVolume(float v) { m_runtimeVolume.store(v, std::memory_order_relaxed); }
    float GetVolume() const { return m_runtimeVolume.load(std::memory_order_relaxed); }

    uint64_t NextPcmData(void* pData, uint32_t frameCount) override {
        // first
        if (! m_curActive) {
            Switch();
        }
        if (! m_curActive) return 0;

        // loop
        uint64_t frameReads = m_curActive->NextPcmData(pData, frameCount);
        if (frameReads == 0) {
            Switch();
            if (! m_curActive) return 0;
            frameReads = m_curActive->NextPcmData(pData, frameCount);
        }
        // volume (use runtime volume for script-driven updates)
        {
            float      vol        = m_runtimeVolume.load(std::memory_order_relaxed);
            float*     pData_float = static_cast<float*>(pData);
            const auto num         = frameReads * m_desc.channels;
            for (uint i = 0; i < num; i++, pData_float++) {
                (*pData_float) *= vol;
            }
        }
        return frameReads;
    };
    void PassDesc(const Desc& d) override { m_desc = d; }
    void Switch() {
        std::string path   = m_soundPaths[LoopIndex()];
        LOG_INFO("audio Switch: loading '%s' (channels=%u, sampleRate=%u)",
                 path.c_str(), m_desc.channels, m_desc.sampleRate);
        auto stream = vfs.Open("/assets/" + path);
        if (! stream) {
            LOG_ERROR("audio file not found: %s", path.c_str());
            m_curActive.reset();
            return;
        }
        m_curActive = audio::CreateSoundStream(std::move(stream), m_desc);
    }
    uint32_t LoopIndex() {
        m_curIndex++;
        if (m_curIndex == m_soundPaths.size()) m_curIndex = 0;
        return m_curIndex;
    }

private:
    fs::VFS& vfs;
    Config   m_config;
    Desc     m_desc;
    uint32_t m_curIndex { 0 };

    const std::vector<std::string> m_soundPaths;
    std::unique_ptr<SoundStream>   m_curActive;
    std::atomic<float>             m_runtimeVolume;
};

WPSoundStream* WPSoundParser::Parse(const wpscene::WPSoundObject& obj, fs::VFS& vfs,
                                    audio::SoundManager& sm) {
    float vol = obj.volume > 1.0f ? 1.0f : obj.volume;

    // Skip sound objects with zero volume (e.g. click sounds with default volume=0)
    if (vol <= 0.001f && !obj.hasVolumeScript) {
        LOG_INFO("sound '%s': skipped (volume=%.3f, no script)", obj.name.c_str(), vol);
        return nullptr;
    }

    LOG_INFO("sound '%s': volume=%.3f hasScript=%d mode=%s files=%zu",
             obj.name.c_str(), vol, (int)obj.hasVolumeScript,
             obj.playbackmode.c_str(), obj.sound.size());

    WPSoundStream::Config config { .maxtime = obj.maxtime,
                                   .mintime = obj.mintime,
                                   .volume  = vol,
                                   .mode    = ToPlaybackMode(obj.playbackmode) };

    auto ss = std::make_unique<WPSoundStream>(obj.sound, vfs, config);
    WPSoundStream* rawPtr = ss.get();
    sm.MountStream(std::move(ss));
    return rawPtr;
}

void WPSoundParser::SetStreamVolume(void* stream, float volume) {
    if (stream) static_cast<WPSoundStream*>(stream)->SetVolume(volume);
}

} // namespace wallpaper
