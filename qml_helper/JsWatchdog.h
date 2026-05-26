#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include <QJSEngine>

namespace scenebackend
{

// ---------------------------------------------------------------------------
// A3-T2 — QJSEngine watchdog (untrusted SceneScript on the GUI thread).
//
// Author SceneScript runs synchronously on the compositor GUI thread inside
// QJSEngine::call().  A non-terminating script (`while(true){}`) never returns,
// so the GUI thread — and with it the whole desktop — freezes.  QJSEngine has no
// instruction/time budget; the only lever is QJSEngine::setInterrupted(true),
// which aborts the in-flight evaluation (it unwinds as a JS exception).
//
// A same-thread QTimer cannot fire while the GUI thread is blocked inside
// .call(), so the budget MUST be enforced from a SEPARATE thread.  JsWatchdog
// owns one such monitor thread: arm() records a deadline immediately before a
// .call(); if disarm() hasn't cleared it within the budget, the monitor calls
// setInterrupted(true) on the armed engine, aborting the runaway script.
//
// Engine-pointer safety: arm() publishes the engine pointer into an atomic that
// the monitor reads under its mutex; disarm() clears it.  stop() joins the
// thread, so it blocks until any in-flight setInterrupted() call has returned —
// callers MUST stop() the watchdog BEFORE deleting the QJSEngine, which makes a
// post-delete setInterrupted() impossible.  The atomic null-check is the
// belt-and-suspenders for the disarmed window.
// ---------------------------------------------------------------------------

// Pure, unit-testable interrupt policy (mirrors ScriptLoopGate.h's split so it
// can be tested without the monitor thread or a Vulkan-backed SceneObject):
// after K consecutive budget-exceeding interrupts, the offending wallpaper's
// property timer is disabled (it stops animating instead of freezing the shell).
// A clean tick resets the counter.  The loop disables iff the run of
// consecutive interrupts has reached K (K<=0 disables on the first interrupt).
inline bool shouldDisableAfterInterrupts(int consecutiveInterrupts, int k) {
    return consecutiveInterrupts >= k;
}

class JsWatchdog {
public:
    // Default per-tick wall-clock budget.  Generous so legitimate heavy
    // first-frame scripts (puppet/rig init) are never interrupted — the
    // watchdog is a runaway safety net, not a scheduler.
    static constexpr int64_t kDefaultBudgetMs = 250;
    // Disable the wallpaper's property timer after this many consecutive
    // interrupts (then log once).  A clean tick resets the run.
    static constexpr int kDisableAfterInterrupts = 5;

    JsWatchdog() = default;
    ~JsWatchdog() { stop(); }

    JsWatchdog(const JsWatchdog&)            = delete;
    JsWatchdog& operator=(const JsWatchdog&) = delete;

    // Lazily start the monitor thread.  Idempotent — and safe under concurrent
    // callers: the mutex serialises the running-flag check and thread spawn so
    // two concurrent start() calls can't both pass the check and both
    // move-assign over a joinable m_thread (which would call std::terminate).
    // After a matched stop() the second start() validly re-spawns since stop()
    // joins m_thread before returning.
    void start() {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_running.load(std::memory_order_relaxed)) return;
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread([this] {
            monitorLoop();
        });
    }

    // Stop and join the monitor thread.  Idempotent.  MUST be called before the
    // armed QJSEngine is destroyed (join() guarantees no setInterrupted() is in
    // flight afterwards).
    void stop() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (! m_running.load(std::memory_order_acquire) && ! m_thread.joinable()) return;
            m_running.store(false, std::memory_order_release);
            m_armed  = false;
            m_engine = nullptr;
        }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }

    // Arm a deadline `budgetMs` from now for `engine`.  Called on the GUI thread
    // immediately before QJSEngine::call().  No-op (no protection) if start()
    // was never called or engine is null.
    void arm(QJSEngine* engine, int64_t budgetMs = kDefaultBudgetMs) {
        if (engine == nullptr) return;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_engine   = engine;
            m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
            m_armed    = true;
            m_fired    = false;
        }
        m_cv.notify_all();
    }

    // Disarm after the call returns.  Returns true if the watchdog fired the
    // interrupt for the just-finished call (so the caller can clear the engine's
    // interrupted flag and bump its consecutive-interrupt counter).
    bool disarm() {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_armed    = false;
        m_engine   = nullptr;
        bool fired = m_fired;
        m_fired    = false;
        return fired;
    }

private:
    void monitorLoop() {
        std::unique_lock<std::mutex> lk(m_mtx);
        while (m_running.load(std::memory_order_acquire)) {
            if (! m_armed) {
                // Sleep until armed (or shutdown).
                m_cv.wait(lk, [this] {
                    return m_armed || ! m_running.load(std::memory_order_acquire);
                });
                continue;
            }
            auto deadline = m_deadline;
            // Wait until the deadline, an explicit disarm/re-arm, or shutdown.
            if (m_cv.wait_until(lk, deadline, [this, deadline] {
                    return ! m_armed || ! m_running.load(std::memory_order_acquire) ||
                           m_deadline != deadline;
                })) {
                // Disarmed / re-armed / shutting down before the deadline — loop.
                continue;
            }
            // Deadline expired while still armed: interrupt the running JS.
            // setInterrupted is the documented cross-thread abort for QJSEngine.
            if (m_armed && m_engine != nullptr) {
                interruptEngine(m_engine);
                m_fired = true;
                m_armed = false; // one interrupt per arm; caller clears the flag
            }
        }
    }

    // Inline so a real armed JsWatchdog is linkable in scenescript_tests
    // (which links Qt6::Qml but NOT SceneBackend.cpp).  One-liner with no
    // Vulkan/wpUtils dependency — needs only the full QJSEngine type, included
    // above.  Runs on the monitor thread; setInterrupted is the documented
    // cross-thread abort for an in-flight QJSEngine evaluation.
    static void interruptEngine(QJSEngine* engine) {
        if (engine) engine->setInterrupted(true);
    }

    std::thread                           m_thread;
    std::mutex                            m_mtx;
    std::condition_variable               m_cv;
    std::atomic<bool>                     m_running { false };
    bool                                  m_armed { false };
    bool                                  m_fired { false };
    QJSEngine*                            m_engine { nullptr };
    std::chrono::steady_clock::time_point m_deadline {};
};

} // namespace scenebackend
