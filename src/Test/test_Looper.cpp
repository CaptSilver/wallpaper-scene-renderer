#include <doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "Looper/Looper.hpp"

using wallpaper::looper::Looper;
using wallpaper::looper::status_t;

// A Looper owns the wallpaper worker thread. ~Looper runs from ~MainHandler on
// plasmashell's GUI thread, so any path that lets a joinable std::thread reach
// its destructor is a std::terminate that takes the user's whole session down.

TEST_SUITE("Looper lifecycle") {
    TEST_CASE("stop() immediately after start() still joins the worker") {
        // start() launches the thread but left the WORKER to set m_running. A
        // stop() landing inside that window read false, returned without ever
        // swapping/joining m_thread, and left the joinable thread to ~Looper.
        // Loop to land inside the window rather than depend on one scheduling.
        int completed = 0;
        for (int i = 0; i < 200; i++) {
            auto looper = std::make_shared<Looper>();
            looper->setName("test-looper");
            REQUIRE(looper->start() == status_t::OK);
            looper->stop();
            completed++;
        }
        CHECK(completed == 200);
    }

    TEST_CASE("destroying a started looper without an explicit stop() does not terminate") {
        int completed = 0;
        for (int i = 0; i < 200; i++) {
            auto looper = std::make_shared<Looper>();
            looper->setName("test-looper-dtor");
            REQUIRE(looper->start() == status_t::OK);
            // Dropping the last reference runs ~Looper -> stop(). This is the
            // production shape: ~MainHandler drops the looper.
            completed++;
        }
        CHECK(completed == 200);
    }

    TEST_CASE("start() on an already-started looper reports INVALID_OPERATION") {
        // The running flag must be observable the moment start() returns, or a
        // second start() spawns a second thread over m_thread and orphans the first.
        auto looper = std::make_shared<Looper>();
        looper->setName("test-looper-double-start");
        REQUIRE(looper->start() == status_t::OK);
        CHECK(looper->start() == status_t::INVALID_OPERATION);
        looper->stop();
    }

    TEST_CASE("worker never touches Looper state after the owner's last "
              "reference is gone") {
        // Reproduces the race the 200-iteration destructor test above only hit
        // once in two hundred runs: the worker locks a strong reference to the
        // Looper, then (in the old code) let that reference go out of scope
        // and kept using a raw pointer copied out of it. If the owner dropped
        // its own last reference while the worker held the only other one,
        // the worker's copy was the one that took the refcount to zero,
        // running ~Looper() right there and freeing the object the raw
        // pointer still pointed to. testHook_workerLockedRef pauses the
        // worker exactly inside that window so the race is forced every time
        // instead of hoped for.
        std::atomic<bool> workerHoldingRef { false };
        std::atomic<bool> mayProceed { false };
        std::atomic<bool> workerExited { false };

        Looper::testHook_workerLockedRef = [&]() {
            workerHoldingRef.store(true, std::memory_order_release);
            while (! mayProceed.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        };
        // Fires as the worker's OS thread function returns. On this test's
        // self-stop path nothing else can join that thread (stop() detaches
        // it, see Looper.cpp), so a fixed sleep here would only ever be a
        // guess at how long ~Looper()/stop()/the final LOG_INFO take under
        // whatever load the box is under -- TSan flagged exactly that gap
        // once, "as if synchronized via sleep", racing this test's stack
        // against a later, unrelated test's.  Wait on this instead.
        Looper::testHook_workerAboutToExit = [&]() {
            workerExited.store(true, std::memory_order_release);
        };
        struct HookGuard {
            ~HookGuard() {
                Looper::testHook_workerLockedRef   = nullptr;
                Looper::testHook_workerAboutToExit = nullptr;
            }
        } hookGuard;

        auto looper = std::make_shared<Looper>();
        looper->setName("race-test-looper");
        REQUIRE(looper->start() == status_t::OK);

        // Wait until the worker is parked inside the hook, holding the only
        // strong reference besides ours.
        while (! workerHoldingRef.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        // Drop our reference while the worker still holds its own: the
        // worker's copy is now the last one alive.
        looper.reset();

        // Let the worker continue. Its locked reference goes out of scope
        // next, taking the refcount to zero on the worker thread itself,
        // which runs ~Looper() -> stop() -> detach() right there (self-stop).
        mayProceed.store(true, std::memory_order_release);

        // We no longer own the thread (looper is gone) and it detached
        // itself, so there is no join that would tell us it's done -- wait
        // for its own exit signal instead of guessing at a sleep duration.
        while (! workerExited.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}
