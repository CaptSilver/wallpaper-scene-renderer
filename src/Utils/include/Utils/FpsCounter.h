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
    // it mid-process without sub-process isolation.  16 buckets with
    // non-uniform, geometrically-growing edges (see kBucketEdgesMs in
    // FpsCounter.cpp) so a 54ms hitch and a 2000ms stall no longer land on
    // the same reported percentile; bucket 15 is an unbounded catch-all
    // reported via the tracked exact max instead of a synthetic edge.
    // CollectHistogram returns a snapshot and resets the counters in place
    // so callers (e.g. a 30 s log cadence on the render thread) get rolling
    // windows rather than cumulative totals.
    //
    // DiscardNextInterval() marks the NEXT RegisterFrame() call's interval
    // as known-long (scene load, compileRenderGraph) so it is timestamped
    // but excluded from both the histogram and the tracked max — otherwise
    // a multi-second scene switch reports as the frame-time max forever
    // until the next 30s log window. m_discardNext defaults true so the
    // construction-to-first-frame gap is excluded without a separate caller.
    //
    // Threading: m_buckets / m_maxMs / m_discardNext are atomic so a
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

    // Excludes the NEXT RegisterFrame() interval from the histogram and the
    // tracked max — call before a known-long gap (e.g. scene load) so it
    // doesn't get reported as a frame-time hitch.
    void DiscardNextInterval() { m_discardNext.store(true, std::memory_order_relaxed); }

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
    // Set by DiscardNextInterval(), consumed (exchanged back to false) by the
    // next RegisterFrame().  Defaults true so the construction-to-first-frame
    // gap is excluded without a dedicated first-call flag.
    std::atomic<bool> m_discardNext { true };
};

} // namespace wallpaper
