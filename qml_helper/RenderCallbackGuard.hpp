#pragma once

#include <mutex>

namespace wek::qml_helper
{

// TextureNode hands the wallpaper render thread a handful of callbacks
// (redraw, first-frame, video-decode-failed, scene-load-failed) that keep
// firing for as long as the underlying SceneWallpaper is alive. That can
// outlast the TextureNode itself: Qt Quick's scenegraph node cleanup and the
// QML item's own C++ destruction run on independent schedules (the node can
// be gone well before the owning item's deleteLater actually fires), and
// during that gap the render thread has no way to know the callback's target
// is gone -- a callback built around a raw `this` just calls through a
// dangling pointer.
//
// CallbackGuard replaces the raw capture. The render thread holds a
// shared_ptr<CallbackGuard<T>> instead of T*, and only ever reaches T through
// invoke(), which runs under the guard's own lock. invalidate() -- called at
// the very start of ~T(), before any other member is torn down -- clears the
// pointer under that same lock, so it can never overlap with a call already
// inside invoke(): either invoke() observes the object and runs to
// completion before invalidate() can take the lock, or invalidate() gets
// there first and invoke() sees nothing. Either way, nothing ever touches T
// once its destructor has started, and invalidate() doesn't return until any
// call already in flight has finished -- so by the time ~T() moves on to
// free real state, no other thread can be mid-callback.
template<typename T>
class CallbackGuard {
public:
    explicit CallbackGuard(T* owner): m_owner(owner) {}

    // Call exactly once, from ~T(), before touching any other member.
    // Blocks until a call already past invoke()'s null-check has returned.
    void invalidate() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_owner = nullptr;
    }

    // Runs fn(owner) if T hasn't been invalidated yet. Returns whether fn
    // ran, so callers (and tests) can tell without a side channel.
    template<typename Fn>
    bool invoke(Fn&& fn) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (! m_owner) return false;
        fn(m_owner);
        return true;
    }

private:
    std::mutex m_mutex;
    T*         m_owner;
};

} // namespace wek::qml_helper
