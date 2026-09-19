#include <doctest.h>

#include "RenderCallbackGuard.hpp"

#include <atomic>
#include <chrono>
#include <thread>

// Regression coverage for the TextureNode redraw-callback use-after-free: the
// wallpaper render thread kept a raw `this` in a std::function and called
// through it after the QObject it pointed at was gone (see
// RenderCallbackGuard.hpp for the full mechanism). These pin the guard's two
// load-bearing properties in isolation, with no Qt/Vulkan involved: a call
// after invalidate() is a no-op, and invalidate() cannot return while a call
// is still inside the guarded region.
TEST_SUITE("RenderCallbackGuard") {
    using wek::qml_helper::CallbackGuard;

    struct Victim {
        int touched = 0;
    };

    TEST_CASE("invoke runs the callback against the live owner") {
        Victim                v;
        CallbackGuard<Victim> guard(&v);

        bool ran = guard.invoke([](Victim* p) {
            p->touched++;
        });

        CHECK(ran);
        CHECK(v.touched == 1);
    }

    TEST_CASE("invoke after invalidate is a no-op") {
        Victim                v;
        CallbackGuard<Victim> guard(&v);

        guard.invalidate();
        bool ran = guard.invoke([](Victim* p) {
            p->touched++;
        });

        CHECK_FALSE(ran);
        CHECK(v.touched == 0);
    }

    TEST_CASE("invalidate is safe to call more than once") {
        Victim                v;
        CallbackGuard<Victim> guard(&v);

        guard.invalidate();
        guard.invalidate();
        CHECK_FALSE(guard.invoke([](Victim* p) {
            p->touched++;
        }));
    }

    // The property that actually closes the bug: a destructor calling
    // invalidate() must never return while another thread is still inside a
    // callback that reached in via invoke() -- otherwise the destructor can
    // go on to free the object underneath that still-running callback.
    //
    // Allocated with `new`, not make_shared: a make_shared control block is
    // co-allocated with the object, so a dangling access after a would-be
    // free often lands on still-mapped memory and neither crashes nor trips
    // ASAN -- the bug would look fixed when it was only unobserved.  Nothing
    // here actually frees `v` while callback_thread is running (that's the
    // whole point of the guard), so this is exercising the guard's
    // synchronization, not relying on ASAN to catch a live race.
    TEST_CASE("invalidate blocks until an in-flight invoke completes") {
        Victim*               v = new Victim();
        CallbackGuard<Victim> guard(v);

        std::atomic<bool> callback_entered { false };
        std::atomic<bool> may_finish_callback { false };
        std::atomic<bool> callback_completed { false };
        std::atomic<bool> invalidate_attempted { false };
        std::atomic<bool> invalidate_returned { false };

        // Holds the guard's lock from callback_entered=true until
        // may_finish_callback flips, so the test controls exactly how long
        // invalidate() has to wait.
        std::thread callback_thread([&] {
            guard.invoke([&](Victim* p) {
                callback_entered.store(true);
                while (! may_finish_callback.load()) {
                    std::this_thread::yield();
                }
                p->touched++;
                callback_completed.store(true);
            });
        });

        while (! callback_entered.load()) {
            std::this_thread::yield();
        }

        std::thread invalidate_thread([&] {
            invalidate_attempted.store(true);
            guard.invalidate();
            invalidate_returned.store(true);
        });

        // Give invalidate() a generous window to reach (and block on) the
        // guard's mutex before we let the callback proceed. A correct
        // implementation cannot return in this window; only checked as a
        // best-effort strengthening of the test, not the pass/fail crux.
        while (! invalidate_attempted.load()) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK_FALSE(invalidate_returned.load());
        CHECK_FALSE(callback_completed.load());

        // Release the callback and let both threads finish. The real
        // assertion: by the time invalidate() has returned, the in-flight
        // callback is guaranteed to have already completed -- this part
        // holds regardless of scheduling, because it's enforced by the
        // guard's own mutex rather than by timing.
        may_finish_callback.store(true);
        invalidate_thread.join();
        callback_thread.join();

        CHECK(invalidate_returned.load());
        CHECK(callback_completed.load());
        CHECK(v->touched == 1);

        // No further callback can reach v, so freeing it now is safe.
        delete v;
        CHECK_FALSE(guard.invoke([](Victim* p) {
            p->touched++;
        }));
    }
}
