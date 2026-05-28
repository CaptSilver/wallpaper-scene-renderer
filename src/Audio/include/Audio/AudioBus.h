#pragma once
#include <memory>

namespace wallpaper
{
namespace audio
{

class AudioAnalyzer;

// Process-singleton audio fan-out: one AudioCapture + AudioAnalyzer pair
// shared across every subscriber (Scene wallpaper, Web wallpaper, future
// MPV taps).  Each subscriber Acquires a shared_ptr<AudioAnalyzer>; the
// underlying capture stays alive while any returned shared_ptr is alive
// and is torn down (via a custom deleter on the analyzer's shared_ptr)
// when the last subscriber drops.
//
// Process() ownership stays single-consumer via a bus-internal 60Hz
// thread; subscribers only read the spectrum via the analyzer's
// documented lock-free APIs (GetRawSpectrum / GetSpectrum16Left etc.).
// This preserves the MPSC + lock-free-read invariants while still
// running a single FFT pipeline for the whole process.
//
// Acquire is thread-safe.  When wantSystemCapture is true and capture
// init fails (no PulseAudio / PipeWire monitor source available) the
// returned analyzer is still valid — it just won't see system audio.
// Subscribers detect that case via the analyzer's HasData()/spectrum
// being empty (matching the pre-bus qWarning fall-through path).
class AudioBus {
public:
    // Returns the shared analyzer.  wantSystemCapture=true requests the
    // PulseAudio/PipeWire monitor stream (lazily opened on the first
    // true-Acquire).  Returns a usable analyzer even when capture init
    // fails — subscribers that need real audio detect via HasData().
    static std::shared_ptr<AudioAnalyzer> Acquire(bool wantSystemCapture);

    // True when an AudioCapture (or the WEK_TEST_AUDIO_NULL_CAPTURE
    // stand-in) is currently feeding the shared analyzer.  Subscribers
    // use this after Acquire(true) to decide whether to fall back to the
    // SoundManager playback tap.  Returns false when no subscriber has
    // yet requested system capture, or when capture-init failed.
    static bool HasSystemCapture();

    // Test-only inspectors.  Implementation detail — never call from
    // production code.
    static bool TEST_isExpired();
    static int  TEST_getInitCount();
    // Joins the background Process thread (if any) and clears all bus
    // state.  Required between doctest cases — the static analyzer's
    // custom deleter joins the thread asynchronously, so without this
    // sync point a subsequent TEST_isExpired() can race against an
    // in-flight teardown.
    static void TEST_resetForNextCase();
};

} // namespace audio
} // namespace wallpaper
