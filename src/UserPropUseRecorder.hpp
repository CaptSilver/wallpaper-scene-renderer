#pragma once

// Which user properties the parser read into something the live scene cannot
// edit in place.
//
// A property can feed several fields. Some of them have a runtime mechanism
// (an object's or effect's `visible`, a text layer's pointsize, a shader
// constant bound to a uniform); the rest are consumed once while parsing.
// Applying a change live is only correct if EVERY read of the property is of
// the first kind, so the parser records the others here, as it resolves them.
// The reads a runtime mechanism does cover are wrapped in a LiveBoundReadScope
// and never reach the record.
//
// Parsing is single-threaded, so a thread_local pointer is enough, same as
// g_currentUserProperties. A parallel object parse would need per-thread
// recorders merged at the end.

#include <map>
#include <set>
#include <string>
#include <string_view>

namespace wallpaper
{

struct UserPropUseRecorder {
    // prop -> the fields it was read for (e.g. "color", "parallaxDepth"). The
    // field names only feed the reload log line; callers decide on the keys.
    std::map<std::string, std::set<std::string>> uses;
    // > 0 while the read in progress is covered by a runtime mechanism.
    int liveDepth { 0 };

    void record(std::string_view prop, std::string_view field) {
        if (liveDepth > 0 || prop.empty()) return;
        uses[std::string(prop)].insert(std::string(field.empty() ? "value" : field));
    }
};

inline thread_local UserPropUseRecorder* g_userPropUseRecorder = nullptr;

// Records into `recorder` for the lifetime of the scope; nests like
// UserPropertiesScope.
class UserPropUseRecordingScope {
public:
    explicit UserPropUseRecordingScope(UserPropUseRecorder* recorder)
        : m_previous(g_userPropUseRecorder) {
        g_userPropUseRecorder = recorder;
    }
    ~UserPropUseRecordingScope() { g_userPropUseRecorder = m_previous; }
    UserPropUseRecordingScope(const UserPropUseRecordingScope&)            = delete;
    UserPropUseRecordingScope& operator=(const UserPropUseRecordingScope&) = delete;

private:
    UserPropUseRecorder* m_previous;
};

// Marks the reads made inside it as covered by a runtime mechanism, so the
// recorder ignores them. A no-op when no recorder is active.
class LiveBoundReadScope {
public:
    LiveBoundReadScope(): m_recorder(g_userPropUseRecorder) {
        if (m_recorder) ++m_recorder->liveDepth;
    }
    ~LiveBoundReadScope() {
        if (m_recorder) --m_recorder->liveDepth;
    }
    LiveBoundReadScope(const LiveBoundReadScope&)            = delete;
    LiveBoundReadScope& operator=(const LiveBoundReadScope&) = delete;

private:
    UserPropUseRecorder* m_recorder;
};

// Record a static use if a recorder is active.
inline void recordUserPropStaticUse(std::string_view prop, std::string_view field) {
    if (g_userPropUseRecorder) g_userPropUseRecorder->record(prop, field);
}

} // namespace wallpaper
