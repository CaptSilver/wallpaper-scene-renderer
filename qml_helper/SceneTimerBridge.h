#pragma once

#include <QObject>
#include <QTimer>
#include <QJSEngine>
#include <QJSValue>
#include <algorithm>
#include <functional>
#include <unordered_map>

namespace scenebackend {

/// Provides setTimeout/setInterval/clearTimeout/clearInterval to QJSEngine.
/// Each timer is backed by a real QTimer for accurate wall-clock timing.
class SceneTimerBridge : public QObject {
    Q_OBJECT
public:
    /// Called after each timer callback fires.
    /// Parameters: timer id, whether the callback threw, error message (if any).
    using PostFireFn = std::function<void(int id, bool error, const QString& msg)>;

    explicit SceneTimerBridge(QJSEngine* engine, QObject* parent = nullptr,
                              PostFireFn postFire = {})
        : QObject(parent), m_engine(engine), m_postFire(std::move(postFire)) {}

    ~SceneTimerBridge() override { clearAll(); }

    Q_INVOKABLE int createTimer(QJSValue callback, int delay, bool repeat) {
        int id = m_nextId++;
        auto* timer = new QTimer(this);
        timer->setSingleShot(!repeat);
        timer->setInterval(std::max(delay, 1));
        m_timers[id] = { timer, std::move(callback) };

        connect(timer, &QTimer::timeout, this, [this, id]() {
            auto it = m_timers.find(id);
            if (it == m_timers.end()) return;
            bool error = false;
            QString errorMsg;
            if (it->second.callback.isCallable()) {
                QJSValue r = it->second.callback.call();
                if (r.isError()) {
                    error = true;
                    errorMsg = r.toString();
                }
            }
            if (m_postFire) m_postFire(id, error, errorMsg);
            // Re-lookup: postFire may have cleared timers
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
    };
    QJSEngine*                          m_engine;
    std::unordered_map<int, TimerEntry> m_timers;
    int                                 m_nextId { 1 };
    PostFireFn                          m_postFire;
};

} // namespace scenebackend
