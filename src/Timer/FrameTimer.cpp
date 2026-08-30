#include "FrameTimer.hpp"
#include "Utils/Logging.h"

#include <numeric>

using namespace wallpaper;
using micros = std::chrono::microseconds;
using namespace std::chrono;

namespace
{
i64 nsOf(ThreadTimer::Clock::time_point tp) {
    return duration_cast<nanoseconds>(tp.time_since_epoch()).count();
}
ThreadTimer::Clock::time_point tpOf(i64 ns) {
    return ThreadTimer::Clock::time_point(
        duration_cast<ThreadTimer::Clock::duration>(nanoseconds(ns)));
}
} // namespace

FrameTimer::FrameTimer(std::function<void()> cb)
    : m_callback(cb),
      m_frametime(micros(66'666)),
      m_timer([this](ThreadTimer::Clock::time_point now) { return OnWake(now); }) {
    SetRequiredFps(15);
}

FrameTimer::~FrameTimer() {};

ThreadTimer::Clock::time_point FrameTimer::OnWake(ThreadTimer::Clock::time_point now_tp) {
    const i64 now = nsOf(now_tp);
    const u64 gen = m_grid_gen.load();
    if (gen != m_seen_gen) {
        // fps/refresh changed or Run() re-anchored: rebuild at now so the
        // change takes effect on this wake, not one stale period later.
        m_seen_gen = gen;
        m_grid     = pacing::MakeGrid(now, m_req_fps.load(), m_refresh_mhz.load());
    }
    const auto plan = pacing::PlanTick(m_grid, now, m_frame_busy_count.load() <= 3);
    if (plan.fire && m_callback) {
        m_frame_busy_count++;
        m_callback();
    }
    if (plan.skipped > 0) m_skipped_ticks.fetch_add(plan.skipped);
    return tpOf(m_grid.DeadlineNs());
}

u16 FrameTimer::RequiredFps() const { return m_req_fps.load(); }
u64 FrameTimer::SkippedTicks() const { return m_skipped_ticks.load(); }

double FrameTimer::FrameTime() const {
    return duration_cast<duration<double>>(m_frametime.load()).count();
}

double FrameTimer::IdeaTime() const {
    const double period = static_cast<double>(m_period_ns.load()) / 1e9;
    const double work   = FrameTime();
    return work > period ? work : period;
}

void FrameTimer::RecomputePeriod() {
    m_period_ns.store(
        pacing::MakeGrid(0, m_req_fps.load(), m_refresh_mhz.load()).ApproxPeriodNs());
}

void FrameTimer::ReseedFrametimeQueue() {
    const auto seed = micros(m_period_ns.load() / 1000);
    std::lock_guard<std::mutex> lk(m_frametime_mutex);
    m_frametime_queue.assign(FRAMETIME_QUEUE_SIZE, seed);
    m_frametime.store(seed);
}

void FrameTimer::SetRequiredFps(u16 value) {
    m_req_fps.store(static_cast<u16>(pacing::ClampFps(value)));
    RecomputePeriod();
    ReseedFrametimeQueue();
    m_grid_gen.fetch_add(1);
    m_timer.Nudge();
}

void FrameTimer::SetOutputRefreshMillihertz(u32 mhz) {
    if (m_refresh_mhz.exchange(mhz) == mhz) return;
    RecomputePeriod();
    m_grid_gen.fetch_add(1);
    m_timer.Nudge();
}

void FrameTimer::FrameBegin() { m_clock = steady_clock::now(); }
void FrameTimer::FrameEnd() {
    const auto now = steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_frametime_mutex);
        m_frametime_queue.push_back(duration_cast<micros>(now - m_clock));
        while (m_frametime_queue.size() > FRAMETIME_QUEUE_SIZE) {
            m_frametime_queue.pop_front();
        }
        m_frametime.store(std::accumulate(m_frametime_queue.begin(),
                                          m_frametime_queue.end(),
                                          duration_cast<micros>(0s)) /
                          m_frametime_queue.size());
    }

    i32 expected = m_frame_busy_count.load();
    while (expected > 0) {
        if (m_frame_busy_count.compare_exchange_weak(expected, expected - 1)) {
            break;
        }
    }
}

void FrameTimer::SetCallback(const std::function<void()>& cb) {
    if (Running()) {
        LOG_ERROR("FrameTimer::SetCallback ignored: timer is running");
        return;
    }
    m_callback = cb;
}

void FrameTimer::Run() {
    // Redundant play() (CMD_STOP(false) has no dedup anywhere up the chain)
    // must not wipe the in-flight budget or re-anchor the grid mid-run.
    if (Running()) return;
    // Reset the in-flight budget: the device-lost pre-draw path can bail
    // without a matching FrameEnd, and 4 leaked slots freeze the wallpaper.
    // Recovery wraps reinit in Stop()/Run(), so this lands exactly there.
    m_frame_busy_count.store(0);
    m_grid_gen.fetch_add(1); // re-anchor at now -> prompt first frame
    m_timer.Nudge();
    m_timer.Start();
}
void FrameTimer::Stop() { m_timer.Stop(); }
bool FrameTimer::Running() const { return m_timer.Running(); }
