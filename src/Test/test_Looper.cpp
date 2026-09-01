#include <doctest.h>

#include <memory>

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
}
