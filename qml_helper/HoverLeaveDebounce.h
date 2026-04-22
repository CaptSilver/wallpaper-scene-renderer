#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace scenebackend
{

// Pure helpers for the hover-leave debounce SceneBackend uses so brief
// cursor wobbles off a hover zone don't fire cursorLeave.  Split out of the
// Qt-heavy hoverMoveEvent so they can be unit-tested end-to-end without
// spinning up a QQuickItem.
//
// Model: a layer enters m_hoveredLayers on the first hit-test that returns
// true (cursorEnter fires).  When the cursor leaves, a leave is SCHEDULED
// (stored in pendingLeaves with a deadline).  If the cursor re-enters
// before the deadline, the pending leave is cancelled.  Otherwise the
// deadline expires and cursorLeave fires.  The key invariant is that the
// deadline is set ONCE per exit — subsequent out-of-layer frames must not
// push the deadline forward, or cursorLeave would never fire.

struct PendingLeave {
    int64_t deadlineMs;
};

struct HoverFrameResult {
    // Names of layers where cursorEnter should fire this frame.
    std::unordered_set<std::string> toEnter;
    // The new value for the caller's m_hoveredLayers set.  Includes layers
    // the cursor is currently over PLUS layers whose pending leaves haven't
    // expired (kept so re-entry during grace doesn't re-fire cursorEnter).
    std::unordered_set<std::string> newHovered;
};

// Process one hover frame.  `pendingLeaves` is updated in place.
inline HoverFrameResult
processHoverFrame(const std::unordered_set<std::string>&         prevHovered,
                  const std::unordered_set<std::string>&         currentHit,
                  std::unordered_map<std::string, PendingLeave>& pendingLeaves, int64_t nowMs,
                  int64_t graceMs) {
    HoverFrameResult r;

    // Layers under the cursor this frame: cancel any pending leave, fire
    // cursorEnter if this is a fresh entry.
    for (const auto& name : currentHit) {
        pendingLeaves.erase(name);
        if (prevHovered.count(name) == 0) {
            r.toEnter.insert(name);
        }
    }

    // Build the new hovered set: start with whatever the cursor currently
    // overlaps, then add back layers that left this frame (or any prior
    // frame during grace) so they stay "hovered" until the deadline.
    r.newHovered = currentHit;
    for (const auto& name : prevHovered) {
        if (currentHit.count(name)) continue; // still over
        if (pendingLeaves.find(name) == pendingLeaves.end()) {
            // First frame we noticed the exit — schedule cursorLeave.
            pendingLeaves[name] = { nowMs + graceMs };
        }
        r.newHovered.insert(name); // keep during grace
    }
    return r;
}

// Return the names whose pending-leave deadline has elapsed.  Caller
// should fire cursorLeave on each and remove them from m_hoveredLayers.
inline std::vector<std::string>
drainExpiredLeaves(std::unordered_map<std::string, PendingLeave>& pendingLeaves, int64_t nowMs) {
    std::vector<std::string> expired;
    for (auto it = pendingLeaves.begin(); it != pendingLeaves.end();) {
        if (nowMs >= it->second.deadlineMs) {
            expired.push_back(it->first);
            it = pendingLeaves.erase(it);
        } else {
            ++it;
        }
    }
    return expired;
}

// Earliest deadline still pending, or 0 if none — useful for arming the
// QTimer to fire exactly when the next leave becomes due.
inline int64_t
nextLeaveDeadlineMs(const std::unordered_map<std::string, PendingLeave>& pendingLeaves) {
    int64_t next = 0;
    for (const auto& kv : pendingLeaves) {
        if (next == 0 || kv.second.deadlineMs < next) next = kv.second.deadlineMs;
    }
    return next;
}

} // namespace scenebackend
