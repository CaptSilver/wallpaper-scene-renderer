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

    // Runs the FFT over accumulated samples and publishes the result to the
    // readers.  SINGLE CONSUMER: exactly one thread may ever call this on a
    // given analyzer.  For the shared analyzer that is the AudioBus 60Hz
    // thread and nobody else — it mutates readPos and the kissfft scratch with
    // no lock, so a second caller garbles whole windows.
    void Process();

    // Read spectrum bands — std140-padded (vec4 stride: value at [i*4], zeros at [i*4+1..3])
    //
    // Callable from any thread while Process() runs elsewhere.  Each getter
    // snapshots the last published frame into per-thread storage and returns a
    // span over that copy, so the FFT thread can never rewrite a span you are
    // holding.  Two consequences worth knowing:
    //   - the span stays valid until the SAME thread calls the SAME getter
    //     again (holding the left and right spans at once is fine);
    //   - two getters can straddle a publish, so left and right may be one
    //     frame apart.  Nothing on screen can tell at 60Hz.
    std::span<const float> GetSpectrum16Left() const;
    std::span<const float> GetSpectrum16Right() const;
    std::span<const float> GetSpectrum32Left() const;
    std::span<const float> GetSpectrum32Right() const;
    std::span<const float> GetSpectrum64Left() const;
    std::span<const float> GetSpectrum64Right() const;

    // Unpadded bands, the form SceneScript's audio buffers want.  Same
    // snapshot rules as the padded getters above; resolution is 16, 32 or 64
    // and channel is 0 (left) or 1 (right).  Anything else returns an empty
    // span.
    std::span<const float> GetRawSpectrum(int resolution, int channel) const;

    // True once at least one FFT window has been published.  Any thread.
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
