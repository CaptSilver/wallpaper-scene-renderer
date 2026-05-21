#pragma once

#include <QObject>
#include <QTimer>
#include <QJSEngine>
#include <QJSValue>
#include <QtGlobal>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <unordered_map>

#include "JsWatchdog.h"

namespace scenebackend
{

/// Provides setTimeout/setInterval/clearTimeout/clearInterval to QJSEngine.
/// Each timer is backed by a real QTimer for accurate wall-clock timing.
///
/// Hardening against untrusted Steam-Workshop SceneScript (A3-T1): every timer
/// runs its callback on the GUI thread, so an author `setInterval(fn, 0)` would
/// otherwise schedule ~1000 GUI-thread wakeups/sec, and a `for(;;) setInterval()`
/// loop would leak QTimers without bound.  Two caps defend the desktop:
///   * kMinTimerMs — floor on the QTimer interval (matches the 8ms engine tick;
///     a sub-8ms timer can't beat the render rate anyway).
///   * kMaxTimers  — hard cap on live timers; createTimer past the cap is
///     rejected (returns a sentinel clearTimer ignores) and logged once.
/// NB: logging here uses Qt's qWarning/qInfo rather than the project LOG_* macros
/// on purpose — this header compiles into scenescript_tests, which links only
/// Qt6::Core+Qml and NOT wpUtils (where WallpaperLog lives), so a LOG_ERROR here
/// would be an undefined symbol in the test binary.  qWarning still routes to the
/// journal under plasmashell.
class SceneTimerBridge : public QObject {
    Q_OBJECT
public:
    /// Minimum backing-QTimer interval (ms).  An author delay below this is
    /// clamped up — sub-8ms cadence can't outpace the 8ms script/render tick.
    static constexpr int kMinTimerMs = 8;
    /// Hard cap on simultaneously-live timers per engine.  Well above any
    /// legitimate scene; createTimer past this is rejected, not enqueued.
    static constexpr std::size_t kMaxTimers = 256;
    /// Sentinel returned by createTimer when a cap rejects the request.  The JS
    /// shims treat the return as an id; clearTimer(kRejectedTimerId) is a no-op
    /// (the id is never inserted into m_timers), so a rejected timer is inert.
    static constexpr int kRejectedTimerId = -1;

    /// Pure clamp applied to every author-supplied delay (split out so it can be
    /// unit-tested deterministically without firing a QTimer): floors at
    /// kMinTimerMs so a `setInterval(fn, 0)` can't schedule sub-8ms wakeups.
    static constexpr int clampInterval(int delay) {
        return delay < kMinTimerMs ? kMinTimerMs : delay;
    }

    /// Called after each timer callback fires.
    /// Parameters: timer id, whether the callback threw, error message (if any).
    using PostFireFn = std::function<void(int id, bool error, const QString& msg)>;

    /// Runs the author callback under the engine watchdog (arm->call->disarm).
    /// Returns the callback result; *outInterrupted is set true if the watchdog
    /// fired (the call was aborted as a JS exception).  When unset, the bridge
    /// falls back to a direct .call() so it stays usable standalone (C++ tests).
    using GuardedCallFn =
        std::function<QJSValue(const std::function<QJSValue()>& call, bool* outInterrupted)>;

    explicit SceneTimerBridge(QJSEngine* engine, QObject* parent = nullptr,
                              PostFireFn postFire = {}, GuardedCallFn guardedCall = {})
        : QObject(parent),
          m_engine(engine),
          m_postFire(std::move(postFire)),
          m_guardedCall(std::move(guardedCall)) {}

    ~SceneTimerBridge() override { clearAll(); }

    Q_INVOKABLE int createTimer(QJSValue callback, int delay, bool repeat) {
        // Reject past the hard cap so a runaway author loop can't leak QTimers
        // (and GUI-thread wakeups) without bound.  Log once to avoid flooding
        // the journal when a tight loop hammers the cap every tick.
        if (m_timers.size() >= kMaxTimers) {
            if (! m_capWarned) {
                m_capWarned = true;
                qWarning("SceneTimerBridge: timer cap %zu reached, rejecting createTimer",
                         kMaxTimers);
            }
            return kRejectedTimerId;
        }
        int   id    = m_nextId++;
        auto* timer = new QTimer(this);
        timer->setSingleShot(! repeat);
        // Clamp the floor to kMinTimerMs (was 1ms): a sub-8ms author timer would
        // schedule far more GUI-thread callbacks than the engine can act on.
        timer->setInterval(clampInterval(delay));
        m_timers[id] = { timer, std::move(callback) };

        connect(timer, &QTimer::timeout, this, [this, id]() {
            auto it = m_timers.find(id);
            if (it == m_timers.end()) return;
            bool    error       = false;
            bool    interrupted = false;
            QString errorMsg;
            if (it->second.callback.isCallable()) {
                // Same arm->call->disarm->clear-interrupt bracket every other
                // GUI-thread JS dispatch gets via SceneObject::callJsGuarded.
                // Fall back to a bare call when no hook is wired (standalone use).
                auto invoke = [&]() -> QJSValue {
                    return it->second.callback.call();
                };
                QJSValue r = m_guardedCall ? m_guardedCall(invoke, &interrupted) : invoke();
                if (r.isError()) {
                    error    = true;
                    errorMsg = r.toString();
                }
            }
            if (m_postFire) m_postFire(id, error, errorMsg);

            // Per-timer interrupt back-off: a runaway INTERVAL would otherwise be
            // interrupted every period forever (a slow-motion freeze).  Mirror the
            // property-tick latch — count consecutive interrupts, reset on a clean
            // fire, and clear the timer once the run reaches kDisableAfterInterrupts.
            it = m_timers.find(id); // re-lookup: postFire may have cleared timers
            if (it != m_timers.end()) {
                if (interrupted) {
                    it->second.consecutiveInterrupts++;
                    if (shouldDisableAfterInterrupts(it->second.consecutiveInterrupts,
                                                     JsWatchdog::kDisableAfterInterrupts)) {
                        if (m_postFire)
                            m_postFire(id,
                                       true,
                                       QStringLiteral("timer callback id=%1 exceeded JS budget "
                                                      "%2 fires running — disabling this timer")
                                           .arg(id)
                                           .arg(it->second.consecutiveInterrupts));
                        it->second.timer->stop();
                        delete it->second.timer;
                        m_timers.erase(it);
                        return;
                    }
                } else {
                    it->second.consecutiveInterrupts = 0;
                }
            }

            // Re-lookup again (the disable branch above may have erased it).
            it = m_timers.find(id);
            if (it != m_timers.end() && it->second.timer->isSingleShot()) {
                delete it->second.timer;
                m_timers.erase(it);
            }
        });

        timer->start();
        return id;
    }

    Q_INVOKABLE void clearTimer(int id) {
        auto it = m_timers.find(id);
        if (it != m_timers.end()) {
            it->second.timer->stop();
            delete it->second.timer;
            m_timers.erase(it);
        }
    }

    void clearAll() {
        for (auto& [id, entry] : m_timers) {
            entry.timer->stop();
            delete entry.timer;
        }
        m_timers.clear();
    }

    int activeCount() const { return static_cast<int>(m_timers.size()); }

private:
    struct TimerEntry {
        QTimer*  timer;
        QJSValue callback;
        // Consecutive watchdog interrupts on THIS timer's callback.  A clean fire
        // resets it; at kDisableAfterInterrupts the timer is cleared so a runaway
        // interval can't pin the CPU forever (A3-T2 per-timer back-off).
        int consecutiveInterrupts { 0 };
    };
    QJSEngine*                          m_engine;
    std::unordered_map<int, TimerEntry> m_timers;
    int                                 m_nextId { 1 };
    PostFireFn                          m_postFire;
    GuardedCallFn                       m_guardedCall;
    // Latch so the timer-cap rejection is logged at most once per bridge — a
    // tight createTimer loop would otherwise spam the journal every tick.
    bool m_capWarned { false };
};

} // namespace scenebackend
