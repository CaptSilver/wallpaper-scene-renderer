#include <doctest.h>
#include <thread>
#include <chrono>

#include "Utils/FpsCounter.h"

using wallpaper::FpsCounter;
using namespace std::chrono_literals;

TEST_SUITE("FpsCounter") {
    TEST_CASE("starts at 0 fps before any frame registered") {
        FpsCounter c;
        CHECK(c.Fps() == 0u);
    }

    TEST_CASE("Fps() stays 0 until the first publish window elapses") {
        // Publish window is 500ms.  Registering a few frames immediately must
        // not move Fps() — the counter only updates the atomic at window end.
        FpsCounter c;
        for (int i = 0; i < 30; i++) c.RegisterFrame();
        CHECK(c.Fps() == 0u);
    }

    TEST_CASE("publishes a non-zero Fps after the window") {
        // Register at ~60Hz for slightly longer than the window, then check.
        // Allow a generous range to absorb scheduler jitter on busy CI.
        FpsCounter c;
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < 600ms) {
            c.RegisterFrame();
            std::this_thread::sleep_for(16ms);
        }
        c.RegisterFrame(); // trigger publish
        unsigned int fps = c.Fps();
        CHECK(fps >= 40u);
        CHECK(fps <= 70u);
    }

    TEST_CASE("Fps() is thread-safe — reader sees consistent value") {
        // Smoke check that concurrent Fps()/RegisterFrame() doesn't tear the
        // u32.  The atomic guarantees this; the test catches accidental
        // regressions if anyone makes m_fps non-atomic again.
        FpsCounter        c;
        std::atomic<bool> stop { false };
        std::thread       reader([&] {
            while (! stop.load()) {
                volatile unsigned int v = c.Fps();
                (void)v;
            }
        });
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < 200ms) {
            c.RegisterFrame();
            std::this_thread::sleep_for(1ms);
        }
        stop.store(true);
        reader.join();
        // We just care that nothing crashed and Fps() is in a sane range.
        CHECK(c.Fps() <= 2000u);
    }
}
