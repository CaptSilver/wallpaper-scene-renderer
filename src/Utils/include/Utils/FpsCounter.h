#pragma once
#include <cstdint>
#include <atomic>
#include <chrono>
#include <array>
#include <functional>

#include "Core/Literals.hpp"

namespace wallpaper
{

// Wall-clock FPS counter — call RegisterFrame() once per actually-rendered
// frame from the render thread.  Every 2 seconds the rolling average gets
// published into the atomic `m_fps`, which the main thread reads via Fps().
// Distinct from FrameTimer::FrameTime() (per-frame render WORK time, ignores
// idle gaps between frames).
class FpsCounter {
public:
    FpsCounter();
    u32  Fps() const { return m_fps.load(std::memory_order_relaxed); }
    void RegisterFrame();

    // Frame-time histogram observability — off unless WEKDE_DEBUG_FRAMETIME
    // is set to a non-empty, non-"0" value at the moment of each frame.
    // The env probe runs per call so operators can toggle telemetry on a
    // running plasmashell by setting the var and restarting; tests can flip
    // it mid-process without sub-process isolation.  16 buckets, 2 ms each
    // — bucket i covers [i*2 .. (i+1)*2) ms, bucket 15 is the 30 ms+
    // catch-all.  CollectHistogram returns a snapshot and resets the
    // counters in place so callers (e.g. a 30 s log cadence on the render
    // thread) get rolling windows rather than cumulative totals.
    //
    // Threading: m_buckets / m_maxMs / m_sampleCount are atomic so a
    // collector thread can safely read while the render thread keeps
    // writing.  Today's only caller runs on the render thread itself
    // (single-writer + same-thread reader), but the atomics future-proof
    // for a debug-overlay thread that polls without coordination.
    struct HistogramSnapshot {
        std::array<u32, 16> buckets;
        u32                 p95_ms;
        u32                 p99_ms;
        u32                 max_ms;
        u32                 sampleCount;
    };
    HistogramSnapshot CollectHistogram();

private:
    bool histogramEnabled() const noexcept;

    std::atomic<u32>                                   m_fps;
    // Render-thread-only: mutated by RegisterFrame() and never read from any
    // other thread. No public accessor is exposed — cross-thread readers go
    // through the atomic m_fps. If a raw count is ever needed from another
    // thread, promote this to std::atomic<u32> and add an explicit-affinity
    // accessor (mirrors m_fps).
    u32                                                m_frameCount;
    std::chrono::time_point<std::chrono::steady_clock> m_startTime;
    // Render-thread-only: timestamp of the previous RegisterFrame() call,
    // used to compute the per-frame interval that gets binned into the
    // histogram.  Not atomic — only the render thread reads / writes it.
    std::chrono::time_point<std::chrono::steady_clock> m_lastFrameTime;

    // Histogram state — only mutated when histogramEnabled() returns true.
    std::array<std::atomic<u32>, 16> m_buckets {};
    std::atomic<u32>                 m_maxMs { 0 };
    std::atomic<u32>                 m_sampleCount { 0 };
};

} // namespace wallpaper
