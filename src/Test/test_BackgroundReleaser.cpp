#include <doctest.h>

#include "BackgroundReleaser.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// BackgroundReleaser takes the last reference to a torn-down scene off the Qt
// render thread (see BackgroundReleaser.hpp).  These pin what TextureNode
// relies on: the object dies on another thread, the caller does not wait for
// it, nothing is dropped at shutdown, and objects die in hand-over order.
TEST_SUITE("BackgroundReleaser") {
    using wek::qml_helper::BackgroundReleaser;

    struct Probe {
        std::thread::id*   diedOn;
        std::atomic<bool>* dead;
        ~Probe() {
            *diedOn = std::this_thread::get_id();
            dead->store(true);
        }
    };

    template<typename Pred>
    bool waitFor(Pred pred) {
        for (int i = 0; i < 2000 && ! pred(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return pred();
    }

    TEST_CASE("the last reference is dropped on the background thread") {
        BackgroundReleaser<Probe> releaser("test:releaser");
        std::thread::id           diedOn;
        std::atomic<bool>         dead { false };
        releaser.release(std::make_shared<Probe>(&diedOn, &dead));
        REQUIRE(waitFor([&] {
            return dead.load();
        }));
        CHECK(diedOn != std::this_thread::get_id());
    }

    TEST_CASE("release does not wait for a slow destructor") {
        struct Slow {
            std::atomic<bool>* done;
            ~Slow() {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                done->store(true);
            }
        };
        BackgroundReleaser<Slow> releaser;
        std::atomic<bool>        done { false };
        const auto               t0 = std::chrono::steady_clock::now();
        releaser.release(std::make_shared<Slow>(&done));
        const auto spent = std::chrono::steady_clock::now() - t0;
        CHECK(spent < std::chrono::milliseconds(100));
        CHECK(waitFor([&] {
            return done.load();
        }));
    }

    TEST_CASE("a reference still held elsewhere keeps the object alive") {
        BackgroundReleaser<Probe> releaser;
        std::thread::id           diedOn;
        std::atomic<bool>         dead { false };
        auto                      obj  = std::make_shared<Probe>(&diedOn, &dead);
        auto                      copy = obj;
        releaser.release(std::move(obj));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK_FALSE(dead.load());
        copy.reset(); // last reference now dies here, on this thread
        CHECK(dead.load());
        CHECK(diedOn == std::this_thread::get_id());
    }

    TEST_CASE("destruction drains everything handed over, in order") {
        std::mutex       m;
        std::vector<int> order;
        struct Tagged {
            int               tag;
            std::mutex*       m;
            std::vector<int>* order;
            ~Tagged() {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                std::lock_guard<std::mutex> lock(*m);
                order->push_back(tag);
            }
        };
        {
            BackgroundReleaser<Tagged> releaser;
            for (int i = 0; i < 20; i++) releaser.release(std::make_shared<Tagged>(i, &m, &order));
        } // ~BackgroundReleaser joins only after the queue is empty
        REQUIRE(order.size() == 20);
        for (int i = 0; i < 20; i++) CHECK(order[i] == i);
    }

    TEST_CASE("null is ignored") {
        BackgroundReleaser<Probe> releaser;
        releaser.release(nullptr);
    }
}
