#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace wek::qml_helper
{

// A script that console.logs the same string every property tick would
// otherwise put one identical journal line per tick, forever -- SceneObject's
// flushJsConsole runs this before every LOG_INFO to turn that into one line
// per burst plus a repeat count.  No QJSEngine dependency: the decision is
// pure text-and-tick-count bookkeeping, so it's tested without pulling in the
// JS engine at all.
class ConsoleFlushDedup {
public:
    // How many ticks a repeat of an already-logged message stays suppressed.
    // 300 ticks is ~10s at the property-script tick (nominally 30Hz -- see
    // SceneObject::m_propFrameCount): long enough that a script busy-logging
    // a per-frame value doesn't reappear every frame, short enough that a
    // wallpaper still actually spamming shows up again within a few seconds
    // instead of going silent for the rest of the session.
    static constexpr int64_t kWindowTicks = 300;

    struct Decision {
        // Whether the caller should emit the log line this call.
        bool shouldLog = false;
        // Valid only when shouldLog is true: how many times this exact
        // message was suppressed since it last logged. Zero for a message
        // seen for the first time.
        int repeatedCount = 0;
    };

    // Call once per buffered console.log message, in flush order. `tick` is
    // the caller's monotonic tick counter (ticks, not wall-clock time -- a
    // paused wallpaper shouldn't age its suppression window).
    Decision recordAndDecide(const std::string& message, int64_t tick) {
        Entry& entry = m_entries[message];
        if (entry.lastLoggedTick < 0 || tick - entry.lastLoggedTick >= kWindowTicks) {
            Decision decision { true, entry.suppressedCount };
            entry.lastLoggedTick  = tick;
            entry.suppressedCount = 0;
            return decision;
        }
        ++entry.suppressedCount;
        return Decision {};
    }

private:
    struct Entry {
        int64_t lastLoggedTick  = -1;
        int     suppressedCount = 0;
    };

    std::unordered_map<std::string, Entry> m_entries;
};

} // namespace wek::qml_helper
