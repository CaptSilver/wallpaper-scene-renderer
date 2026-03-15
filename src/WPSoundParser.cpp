#include "WPSoundParser.hpp"
#include "Audio/SoundManager.h"
#include "Fs/VFS.h"
#include "wpscene/WPSoundObject.h"
#include "Utils/Logging.h"

#include <string>
#include <string_view>
#include <atomic>
#include <cstring>

namespace wallpaper
{

enum class PlaybackMode
{
    Random,
    Loop,
    Single
};

enum class StreamState
{
    Playing,
    Paused,
    Stopped
};

static PlaybackMode ToPlaybackMode(std::string_view s) {
    if (s == "loop")
        return PlaybackMode::Loop;
    else if (s == "random")
        return PlaybackMode::Random;
    else if (s == "single")
        return PlaybackMode::Single;
    return PlaybackMode::Loop;
};

class WPSoundStream : public audio::SoundStream {
public:
    struct Config {
        float        maxtime { 10.0f };
        float        mintime { 0.0f };
        float        volume { 1.0f };
        PlaybackMode mode { PlaybackMode::Loop };
        bool         startsilent { false };
    };
    WPSoundStream(const std::vector<std::string>& paths, fs::VFS& vfs, Config c)
        : vfs(vfs), m_config(c), m_soundPaths(paths),
          m_runtimeVolume(c.volume),
          m_state(c.startsilent ? StreamState::Stopped : StreamState::Playing) {};
    virtual ~WPSoundStream() = default;

    // Thread-safe volume update (called from QML thread via script evaluation)
    void SetVolume(float v) { m_runtimeVolume.store(v, std::memory_order_relaxed); }
    float GetVolume() const { return m_runtimeVolume.load(std::memory_order_relaxed); }

    // Thread-safe playback control (called from QML thread via SceneScript)
    void Play() {
        m_needsReload.store(true, std::memory_order_relaxed);
        m_state.store(StreamState::Playing, std::memory_order_release);
    }
    void Stop() {
        m_state.store(StreamState::Stopped, std::memory_order_release);
    }
    void Pause() {
        m_state.store(StreamState::Paused, std::memory_order_release);
    }
    bool IsPlaying() const {
        return m_state.load(std::memory_order_acquire) == StreamState::Playing;
    }

    uint64_t NextPcmData(void* pData, uint32_t frameCount) override {
        // Always return frameCount (with silence if needed) to prevent auto-removal
        auto state = m_state.load(std::memory_order_acquire);
        if (state != StreamState::Playing) {
            std::memset(pData, 0, frameCount * m_desc.channels * sizeof(float));
            // If stopped, release decoder resources on audio thread
            if (state == StreamState::Stopped && m_curActive) {
                m_curActive.reset();
            }
            return frameCount;
        }

        // Handle Play-after-Stop: reload decoder on audio thread
        if (m_needsReload.exchange(false, std::memory_order_acquire)) {
            m_curActive.reset();
        }

        // First load
        if (! m_curActive) {
            Switch();
        }
        if (! m_curActive) {
            std::memset(pData, 0, frameCount * m_desc.channels * sizeof(float));
            return frameCount;
        }

        uint64_t frameReads = m_curActive->NextPcmData(pData, frameCount);
        if (frameReads == 0) {
            // Track ended
            if (m_config.mode == PlaybackMode::Single) {
                // Single mode: stop after one play-through
                m_state.store(StreamState::Stopped, std::memory_order_release);
                m_curActive.reset();
                std::memset(pData, 0, frameCount * m_desc.channels * sizeof(float));
                return frameCount;
            }
            // Loop/Random: switch to next track
            Switch();
            if (! m_curActive) {
                std::memset(pData, 0, frameCount * m_desc.channels * sizeof(float));
                return frameCount;
            }
            frameReads = m_curActive->NextPcmData(pData, frameCount);
        }

        // Apply volume (use runtime volume for script-driven updates)
        {
            float      vol         = m_runtimeVolume.load(std::memory_order_relaxed);
            float*     pData_float = static_cast<float*>(pData);
            const auto num         = frameReads * m_desc.channels;
            for (uint64_t i = 0; i < num; i++, pData_float++) {
                (*pData_float) *= vol;
            }
        }

        // Zero-fill remainder if partial read (prevents stale audio data)
        if (frameReads < frameCount) {
            float* tail = static_cast<float*>(pData) + frameReads * m_desc.channels;
            std::memset(tail, 0, (frameCount - frameReads) * m_desc.channels * sizeof(float));
        }

        return frameCount;
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
    std::atomic<StreamState>       m_state;
    std::atomic<bool>              m_needsReload { false };
};

WPSoundStream* WPSoundParser::Parse(const wpscene::WPSoundObject& obj, fs::VFS& vfs,
                                    audio::SoundManager& sm) {
    float vol = obj.volume > 1.0f ? 1.0f : obj.volume;

    // Skip sound objects with zero volume (e.g. click sounds with default volume=0)
    // But allow startsilent sounds through (they'll be played later via script)
    if (vol <= 0.001f && !obj.hasVolumeScript && !obj.startsilent) {
        LOG_INFO("sound '%s': skipped (volume=%.3f, no script, not startsilent)", obj.name.c_str(), vol);
        return nullptr;
    }

    LOG_INFO("sound '%s': volume=%.3f hasScript=%d mode=%s startsilent=%d files=%zu",
             obj.name.c_str(), vol, (int)obj.hasVolumeScript,
             obj.playbackmode.c_str(), (int)obj.startsilent, obj.sound.size());

    WPSoundStream::Config config { .maxtime     = obj.maxtime,
                                   .mintime     = obj.mintime,
                                   .volume      = vol,
                                   .mode        = ToPlaybackMode(obj.playbackmode),
                                   .startsilent = obj.startsilent };

    auto ss = std::make_unique<WPSoundStream>(obj.sound, vfs, config);
    WPSoundStream* rawPtr = ss.get();
    sm.MountStream(std::move(ss));
    return rawPtr;
}

void WPSoundParser::SetStreamVolume(void* stream, float volume) {
    if (stream) static_cast<WPSoundStream*>(stream)->SetVolume(volume);
}

void WPSoundParser::StreamPlay(void* stream) {
    if (stream) static_cast<WPSoundStream*>(stream)->Play();
}

void WPSoundParser::StreamStop(void* stream) {
    if (stream) static_cast<WPSoundStream*>(stream)->Stop();
}

void WPSoundParser::StreamPause(void* stream) {
    if (stream) static_cast<WPSoundStream*>(stream)->Pause();
}

bool WPSoundParser::StreamIsPlaying(void* stream) {
    if (stream) return static_cast<WPSoundStream*>(stream)->IsPlaying();
    return false;
}

} // namespace wallpaper
