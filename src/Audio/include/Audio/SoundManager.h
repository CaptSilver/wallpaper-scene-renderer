#pragma once
#include <memory>
#include <cstddef>
#include <cstdint>
#include <functional>
#include "Utils/Logging.h"
#include "Core/NoCopyMove.hpp"

namespace wallpaper
{

namespace fs
{
class IBinaryStream;
}
namespace audio
{
class AudioAnalyzer;

class SoundStream : NoCopy, NoMove {
public:
    struct Desc {
        uint32_t channels;
        uint32_t sampleRate;
    };

public:
    SoundStream()          = default;
    virtual ~SoundStream() = default;

    virtual uint64_t NextPcmData(void* pData, uint32_t frameCount) = 0;
    virtual void     PassDesc(const Desc&)                         = 0;
};
std::unique_ptr<SoundStream> CreateSoundStream(std::shared_ptr<fs::IBinaryStream>,
                                               const SoundStream::Desc&);

class SoundManager : NoCopy, NoMove {
public:
    SoundManager();
    ~SoundManager();
    void MountStream(std::unique_ptr<SoundStream>&&);
    void UnMountAll();
    // Number of streams still mounted.  Callers hold non-owning aliases to the
    // streams they mounted, so this is how they (and the tests) can tell
    // whether an alias still names a live object.
    std::size_t MountedChannelCount() const;
    void        Test(std::shared_ptr<fs::IBinaryStream>);
    bool        Init();
    bool        IsInited() const;
    void        Play();
    void        Pause();

    float Volume() const;
    bool  Muted() const;
    void  SetMuted(bool);
    void  SetVolume(float);

    void SetAudioAnalyzer(std::shared_ptr<AudioAnalyzer> analyzer);

private:
    class impl;
    std::unique_ptr<impl> pImpl;
};
} // namespace audio
} // namespace wallpaper
