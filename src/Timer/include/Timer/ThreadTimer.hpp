#pragma once

#include "Core/Literals.hpp"
#include "Core/NoCopyMove.hpp"

#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

#include <functional>
#include <chrono>

namespace wallpaper
{

// Absolute-deadline wait loop.  on_wake(now) runs on the timer thread on
// every wake — deadline reached, Nudge(), or spurious — and returns the
// next deadline to sleep until.  All pacing policy lives in the caller
// (FrameTimer / FramePacing.hpp); this class owns only the thread.
class ThreadTimer : NoCopy, NoMove {
public:
    using Clock = std::chrono::steady_clock;

    explicit ThreadTimer(std::function<Clock::time_point(Clock::time_point)> on_wake);
    ~ThreadTimer();

    void Start(); // idempotent; the first wake fires immediately
    void Stop();  // synchronous join — no on_wake after Stop() returns
    void Nudge(); // wake the loop now (fps/refresh changed)

    bool Running() const;

private:
    std::function<Clock::time_point(Clock::time_point)> m_on_wake;

    std::mutex m_op_mutex;

    std::thread             m_timer_thread;
    std::mutex              m_cond_mutex;
    std::condition_variable m_condition;

    u64  m_nudge_gen { 0 };  // guarded by m_cond_mutex
    bool m_stop_req { false }; // guarded by m_cond_mutex

    std::atomic<bool> m_running { false };
};

} // namespace wallpaper
