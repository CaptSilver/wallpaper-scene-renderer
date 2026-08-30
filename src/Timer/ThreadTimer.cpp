#include "ThreadTimer.hpp"

#include <cassert>

using namespace wallpaper;

ThreadTimer::ThreadTimer(std::function<Clock::time_point(Clock::time_point)> on_wake)
    : m_on_wake(std::move(on_wake)) {}

ThreadTimer::~ThreadTimer() { Stop(); }

bool ThreadTimer::Running() const { return m_running; }

void ThreadTimer::Start() {
    std::unique_lock<std::mutex> lock(m_op_mutex);

    if (m_running) return;
    {
        std::unique_lock<std::mutex> cl(m_cond_mutex);
        m_stop_req = false;
    }
    // Set before spawning: the thread's exit check must never observe a
    // stale false and quit before the first tick.
    m_running = true;
    m_timer_thread = std::thread([this]() {
        // First wake immediate — init/play/resume rely on a prompt frame.
        auto deadline = Clock::now();
        // seen carries ACROSS iterations and only advances under the lock,
        // before on_wake runs: a Nudge landing while on_wake executes leaves
        // m_nudge_gen != seen, so the next wait returns immediately instead
        // of sleeping out the stale deadline.
        u64 seen = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> cl(m_cond_mutex);
                m_condition.wait_until(cl, deadline, [&] {
                    return m_stop_req || m_nudge_gen != seen;
                });
                if (m_stop_req) return;
                seen = m_nudge_gen;
            }
            deadline = m_on_wake(Clock::now());
        }
    });
}

void ThreadTimer::Stop() {
    std::unique_lock<std::mutex> lock(m_op_mutex);
    assert(std::this_thread::get_id() != m_timer_thread.get_id());

    if (! m_running) return;

    {
        std::unique_lock<std::mutex> cl(m_cond_mutex);
        m_stop_req = true;
        m_condition.notify_all();
    }

    if (m_timer_thread.joinable()) {
        m_timer_thread.join();
    }
    m_running = false;
}

void ThreadTimer::Nudge() {
    std::unique_lock<std::mutex> cl(m_cond_mutex);
    m_nudge_gen += 1;
    m_condition.notify_all();
}
