#pragma once
#include <cstdint>
#include <atomic>
#include <chrono>
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

private:
    std::atomic<u32>                                   m_fps;
    // Render-thread-only: mutated by RegisterFrame() and never read from any
    // other thread. No public accessor is exposed — cross-thread readers go
    // through the atomic m_fps. If a raw count is ever needed from another
    // thread, promote this to std::atomic<u32> and add an explicit-affinity
    // accessor (mirrors m_fps).
    u32                                                m_frameCount;
    std::chrono::time_point<std::chrono::steady_clock> m_startTime;
};

} // namespace wallpaper
