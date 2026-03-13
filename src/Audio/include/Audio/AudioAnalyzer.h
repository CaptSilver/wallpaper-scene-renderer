#pragma once
#include <memory>
#include <cstdint>
#include <span>

namespace wallpaper
{
namespace audio
{

class AudioAnalyzer {
public:
    AudioAnalyzer();
    ~AudioAnalyzer();

    // Called from audio thread (miniaudio data_callback) — lock-free
    void FeedPcm(const float* interleavedStereo, uint32_t frameCount, uint32_t channels);

    // Called once per render frame — runs FFT on accumulated samples
    void Process();

    // Read spectrum bands — std140-padded (vec4 stride: value at [i*4], zeros at [i*4+1..3])
    std::span<const float> GetSpectrum16Left() const;
    std::span<const float> GetSpectrum16Right() const;
    std::span<const float> GetSpectrum32Left() const;
    std::span<const float> GetSpectrum32Right() const;
    std::span<const float> GetSpectrum64Left() const;
    std::span<const float> GetSpectrum64Right() const;

    // Unpadded data for SceneScript (Phase 3)
    std::span<const float> GetRawSpectrum(int resolution, int channel) const;

    bool HasData() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace audio
} // namespace wallpaper
