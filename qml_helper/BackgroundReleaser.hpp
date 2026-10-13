#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <pthread.h>

namespace wek::qml_helper
{

// Drops the last reference to objects on one background thread, in the order
// they were handed over.
//
// A SceneWallpaper's destructor stops its looper threads, persists the
// pipeline cache, destroys the Vulkan device and closes the audio device --
// 200-460 ms measured on an Intel laptop.  Its last reference normally goes
// away in ~TextureNode, which Qt runs on the scenegraph render thread during
// the sync phase, with the GUI thread blocked; every plasmashell window
// (panels included) froze for that long on each wallpaper switch.  Handing
// the reference to this thread moves that work off both.
//
// The destructor drains the queue before joining, so nothing handed over is
// leaked or torn down concurrently with static destruction at exit.
template<typename T>
class BackgroundReleaser {
public:
    explicit BackgroundReleaser(std::string threadName = {})
        : m_name(std::move(threadName)), m_thread([this] {
              run();
          }) {}

    BackgroundReleaser(const BackgroundReleaser&)            = delete;
    BackgroundReleaser& operator=(const BackgroundReleaser&) = delete;

    ~BackgroundReleaser() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_one();
        if (m_thread.joinable()) m_thread.join();
    }

    // Takes the reference; the object is destroyed on the background thread
    // if this was the last one.  Null is ignored.
    void release(std::shared_ptr<T>&& obj) {
        if (! obj) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(obj));
        }
        m_cv.notify_one();
    }

private:
    void run() {
        // Linux caps thread names at 15 characters.
        if (! m_name.empty()) pthread_setname_np(pthread_self(), m_name.substr(0, 15).c_str());
        std::unique_lock<std::mutex> lock(m_mutex);
        while (true) {
            m_cv.wait(lock, [this] {
                return m_stop || ! m_queue.empty();
            });
            if (m_queue.empty()) return; // stopping, and everything is released
            std::shared_ptr<T> obj = std::move(m_queue.front());
            m_queue.pop_front();
            lock.unlock();
            obj.reset(); // the slow part, outside the lock
            lock.lock();
        }
    }

    std::string                    m_name;
    std::mutex                     m_mutex;
    std::condition_variable        m_cv;
    std::deque<std::shared_ptr<T>> m_queue;
    bool                           m_stop { false };
    std::thread                    m_thread; // last: starts once the members above exist
};

} // namespace wek::qml_helper
