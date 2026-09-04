#pragma once
#include <memory>
#include <cstdint>
#include <span>

namespace wallpaper
{
namespace audio
{

// MPSC ring buffer (multi-producer, single-consumer).  Two miniaudio data
// callback threads can call FeedPcm concurrently — AudioCapture's PipeWire
// monitor thread and SoundManager's playback spectrum tap.  Producers
// serialize via an internal mutex around the ring write; the consumer
// (Process) remains lock-free, observing committed writes via a writePos
// acquire load.
class AudioAnalyzer {
public:
    AudioAnalyzer();
    ~AudioAnalyzer();

    // Called from audio thread (miniaudio data_callback).  MPSC: concurrent
    // producers serialize via an internal mutex; consumer is unaffected.
    void FeedPcm(const float* interleavedStereo, uint32_t frameCount, uint32_t channels);

    // Runs the FFT over accumulated samples.  SINGLE CONSUMER: exactly one
    // thread may ever call this on a given analyzer.  For the shared analyzer
    // that is the AudioBus 60Hz thread and nobody else — it mutates readPos,
    // the kissfft scratch and every band array with no lock, so a second
    // caller garbles whole windows.
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

    // Test-only: cumulative count of FFT windows computed since construction.
    // Under 50% overlap, one Process() call may run multiple FFTs (one per
    // FFT_SIZE-stereo-frame stride that fits in the available samples).
    // Exposed for doctest assertions on the overlap contract; production code
    // should use the spectrum APIs above.
    uint64_t WindowsProcessedForTest() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace audio
} // namespace wallpaper
