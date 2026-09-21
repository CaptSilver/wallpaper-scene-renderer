#include "FpsCounter.h"
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <cstdint>

using namespace wallpaper;
using namespace std::chrono;

FpsCounter::FpsCounter()
    : m_fps(0),
      m_frameCount(0),
      m_startTime(steady_clock::now()),
      m_lastFrameTime(m_startTime) {};

// Republish the rolling average every ~half second so consumers (SceneScript
// `engine.fps`) see updates quickly when the user toggles a heavy effect on
// or off, while still averaging across enough frames to smooth out hitches.
constexpr milliseconds kPublishWindow { 500 };

// Upper edge (ms, exclusive) of buckets 0-14. Bucket 15 is the unbounded
// catch-all, reported via the tracked exact max instead of a synthetic
// edge. Geometric growth (~1.48x/bucket) from 2ms to 500ms in 14 steps.
// Defined once here; both RegisterFrame() (binning) and CollectHistogram()
// (percentile reporting) reference this same table so they can't drift apart.
constexpr std::array<u32, 15> kBucketEdgesMs { 2,  3,  5,   7,   11,  16,  24, 35,
                                               52, 77, 114, 169, 251, 372, 500 };

bool FpsCounter::histogramEnabled() const noexcept {
    // Per-call getenv() rather than a cached static-local.  Cost is one
    // libc getenv() (a tiny linear scan of environ) per frame, which is
    // ~ns and well below the noise floor of any frame that's even close
    // to causing a hitch.  In exchange, tests can flip WEKDE_DEBUG_FRAMETIME
    // mid-process without sub-process isolation, and operators can toggle
    // telemetry on a running plasmashell by re-launching it without a
    // rebuild.  If profiling ever shows this on a hot path, promote to a
    // cached std::atomic<int8_t> refreshed every N frames.
    const char* env = std::getenv("WEKDE_DEBUG_FRAMETIME");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
}

void FpsCounter::RegisterFrame() {
    auto now  = steady_clock::now();
    auto diff = now - m_startTime;

    m_frameCount++;
    if (diff > kPublishWindow) {
        m_fps.store((u32)(m_frameCount / duration<double>(diff).count()),
                    std::memory_order_relaxed);
        m_frameCount = 0;
        m_startTime  = now;
    }

    if (histogramEnabled()) {
        // Consume any pending discard first — a caller marked this interval
        // (the one ending "now") as a known-long gap, e.g. scene load. Skip
        // binning it into the histogram/max, but still fall through to the
        // m_lastFrameTime update below so the NEXT interval measures from
        // this point rather than from before the gap.
        if (! m_discardNext.exchange(false, std::memory_order_relaxed)) {
            auto frameDur = duration_cast<milliseconds>(now - m_lastFrameTime).count();
            u32  ms       = (u32)std::max<int64_t>(0, frameDur);

            u32 bucket = 15;
            for (u32 i = 0; i < kBucketEdgesMs.size(); ++i) {
                if (ms < kBucketEdgesMs[i]) {
                    bucket = i;
                    break;
                }
            }
            m_buckets[bucket].fetch_add(1, std::memory_order_relaxed);

            // CAS-loop max — keeps m_maxMs at the running peak across the
            // current collection window.  Failure path reloads `prev` so the
            // exit condition stays correct.
            u32 prev = m_maxMs.load(std::memory_order_relaxed);
            while (ms > prev &&
                   ! m_maxMs.compare_exchange_weak(prev, ms, std::memory_order_relaxed)) {
            }
        }
    }
    m_lastFrameTime = now;
}

FpsCounter::HistogramSnapshot FpsCounter::CollectHistogram() {
    HistogramSnapshot snap {};
    u32               total = 0;
    for (u32 i = 0; i < 16; ++i) {
        snap.buckets[i] = m_buckets[i].exchange(0, std::memory_order_relaxed);
        total           += snap.buckets[i];
    }
    snap.sampleCount = total;
    snap.max_ms      = m_maxMs.exchange(0, std::memory_order_relaxed);

    if (total == 0) return snap;

    // p95/p99 walk: accumulate bucket counts in ascending order; the bucket
    // where the running total crosses the quantile target reports that
    // bucket's upper edge (kBucketEdgesMs), i.e. "no more than N ms for this
    // fraction of frames" — the catch-all bucket 15 has no edge, so it
    // reports the tracked exact max instead.
    u32 p95Target = (u32)((double)total * 0.95);
    u32 p99Target = (u32)((double)total * 0.99);
    u32 acc       = 0;
    for (u32 i = 0; i < 16; ++i) {
        acc += snap.buckets[i];
        u32 edge_ms = (i < kBucketEdgesMs.size()) ? kBucketEdgesMs[i] : snap.max_ms;
        if (snap.p95_ms == 0 && acc >= p95Target) snap.p95_ms = edge_ms;
        if (snap.p99_ms == 0 && acc >= p99Target) {
            snap.p99_ms = edge_ms;
            break;
        }
    }
    return snap;
}
