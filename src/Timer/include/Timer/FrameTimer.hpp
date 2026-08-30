#pragma once

#include "ThreadTimer.hpp"
#include "FramePacing.hpp"
#include <deque>
#include <mutex>

namespace wallpaper
{
class FrameTimer : NoCopy, NoMove {
    constexpr static usize FRAMETIME_QUEUE_SIZE { 5 };

public:
    FrameTimer(std::function<void()> callback = {});
    ~FrameTimer();

    // call before Run(); rejected (loudly) while running
    void SetCallback(const std::function<void()>&);

    void Run();
    void Stop();

    u16    RequiredFps() const;
    bool   Running() const;
    double FrameTime() const;
    double IdeaTime() const;
    u64    SkippedTicks() const;

    void SetRequiredFps(u16);
    // Display refresh in millihertz (59'940 = 59.94Hz); 0 = unknown.  When
    // fps divides the refresh to within 2% the tick grid snaps to whole
    // vsyncs of the true rate.
    void SetOutputRefreshMillihertz(u32 mhz);

    // only used with one render
    void FrameBegin();
    void FrameEnd();

private:
    ThreadTimer::Clock::time_point OnWake(ThreadTimer::Clock::time_point);
    void RecomputePeriod();
    void ReseedFrametimeQueue();

    std::function<void()> m_callback;

    std::mutex                            m_frametime_mutex;
    std::deque<std::chrono::microseconds> m_frametime_queue; // under m_frametime_mutex

    std::atomic<u16> m_req_fps { 15 };
    std::atomic<u32> m_refresh_mhz { 0 };
    std::atomic<i64> m_period_ns { 66'666'667 };
    std::atomic<u64> m_grid_gen { 0 };
    std::atomic<u64> m_skipped_ticks { 0 };

    std::atomic<std::chrono::microseconds> m_frametime;
    std::atomic<i32>                       m_frame_busy_count { 0 };

    // timer-thread only
    pacing::TickGrid m_grid;
    u64              m_seen_gen { 0 };

    ThreadTimer m_timer;

    // render-thread only
    std::chrono::time_point<std::chrono::steady_clock> m_clock;
};
} // namespace wallpaper
