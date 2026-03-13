#pragma once
#include <memory>

namespace wallpaper
{
namespace audio
{

class AudioAnalyzer;

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // Try to open a system audio monitor source (PipeWire/PulseAudio .monitor)
    // Returns true if capture is active
    bool Init(std::shared_ptr<AudioAnalyzer> analyzer);

    // Query whether system capture is working
    bool IsActive() const;

    // Stop capture and release device
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace audio
} // namespace wallpaper
