#include <doctest.h>

#include "Swapchain/TripleSwapchain.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>

using wallpaper::TripleSwapchain;

namespace
{

// One of the three images.  `frame` is the number of the frame last rendered
// into it, so a consumer can tell which frame it was handed.
struct Image {
    int frame { 0 };
    int index { 0 };
};

// TripleSwapchain over three plain images.  The render thread and the Qt
// render thread race on these slots for real; here the test plays both sides
// on one thread and uses `on_publish_midpoint` to run the consumer at a chosen
// point inside renderFrame(), which is how a preempted render thread would
// interleave with eatFrame().
class TestSwapchain : public TripleSwapchain<Image> {
public:
    TestSwapchain() {
        for (int i = 0; i < 3; ++i) m_images[i].index = i;
        m_presented  = &m_images[0];
        m_ready      = &m_images[1];
        m_inprogress = &m_images[2];
    }

    std::uint32_t width() const override { return 1; }
    std::uint32_t height() const override { return 1; }

    // Render frame `n` into the in-progress image and publish it.
    void produce(int n) {
        getInprogress()->frame = n;
        renderFrame();
    }

    // Runs once, the next time renderFrame() has swapped the new frame into
    // `ready` but before it returns.
    std::function<void()> on_publish_midpoint;

protected:
    std::atomic<Image*>& presented() override { return m_presented; }
    std::atomic<Image*>& ready() override { return m_ready; }
    std::atomic<Image*>& inprogress() override {
        // renderFrame() reads inprogress() once for the exchange argument and
        // once more to store the image it got back; that second read sits
        // after the new frame is in `ready` and before the frame is marked
        // available.
        if (on_publish_midpoint && ++m_inprogress_reads_in_publish == 3) {
            auto hook           = std::move(on_publish_midpoint);
            on_publish_midpoint = nullptr;
            hook();
        }
        return m_inprogress;
    }

public:
    void arm_midpoint(std::function<void()> hook) {
        // produce() reads inprogress() once itself before renderFrame().
        m_inprogress_reads_in_publish = 0;
        on_publish_midpoint           = std::move(hook);
    }

private:
    std::array<Image, 3> m_images {};
    std::atomic<Image*>  m_presented { nullptr };
    std::atomic<Image*>  m_ready { nullptr };
    std::atomic<Image*>  m_inprogress { nullptr };
    int                  m_inprogress_reads_in_publish { 0 };
};

// What the Qt side would put on screen: the last image eatFrame() handed over.
// No GPU synchronisation here, so retiring an image needs no signal.
struct Screen {
    TestSwapchain& chain;
    int            shown { 0 };
    int            newest_shown { 0 };

    void repaint() {
        if (Image* img = chain.eatFrame([](Image*) {
            })) {
            shown = img->frame;
            if (shown > newest_shown) newest_shown = shown;
        }
    }
};

} // namespace

TEST_SUITE("TripleSwapchain") {
    TEST_CASE("each repaint shows the newest published frame") {
        TestSwapchain chain;
        Screen        screen { chain };
        for (int n = 1; n <= 5; ++n) {
            chain.produce(n);
            screen.repaint();
            CHECK(screen.shown == n);
        }
    }

    TEST_CASE("a repaint with nothing new published keeps the current frame") {
        TestSwapchain chain;
        Screen        screen { chain };
        chain.produce(1);
        screen.repaint();
        screen.repaint();
        CHECK(screen.shown == 1);
    }

    TEST_CASE("frames published between repaints are skipped, never shown out of order") {
        TestSwapchain chain;
        Screen        screen { chain };
        chain.produce(1);
        chain.produce(2);
        chain.produce(3);
        screen.repaint();
        CHECK(screen.shown == 3);
    }

    // A repaint landing inside renderFrame(), after the new frame is in
    // `ready` but before it is marked available, must not make a later
    // repaint go back in time.  Plasma repaints its desktop window whenever
    // anything on it changes (a rubber-band selection repaints every
    // vsync), so the Qt render thread eats at arbitrary points relative to
    // the wallpaper's render thread.
    TEST_CASE("a repaint inside a publish never makes a later repaint show an older frame") {
        TestSwapchain chain;
        Screen        screen { chain };

        chain.produce(1);
        screen.repaint();
        REQUIRE(screen.shown == 1);

        chain.produce(2); // published, not yet eaten
        chain.arm_midpoint([&] {
            screen.repaint();
        });
        chain.produce(3); // the repaint fires half-way through this publish
        REQUIRE(screen.shown == 3);

        screen.repaint();
        CHECK_MESSAGE(screen.shown >= screen.newest_shown,
                      "repaint went back from frame ",
                      screen.newest_shown,
                      " to frame ",
                      screen.shown);

        chain.produce(4);
        screen.repaint();
        CHECK(screen.shown == 4);
    }
}

namespace
{

// A binary semaphore as Vulkan and GL see it: a signal needs it unsignaled, a
// wait needs a signal to be pending.  Both go wrong silently on a real driver
// (a hang, or content read before it is written), so here they fail the test.
struct FakeBinarySemaphore {
    const char* name;
    int         image;
    bool        signaled { false };

    void signal() {
        CHECK_MESSAGE(! signaled, name, " of image ", image, " signaled while already signaled");
        signaled = true;
    }
    void wait() {
        CHECK_MESSAGE(signaled, name, " of image ", image, " waited with no signal pending");
        signaled = false;
    }
};

using Wait = TripleSwapchain<Image>::InprogressWait;

// Both ends of the handoff with the semaphore traffic the real ones issue:
// the render thread waits on whatever inprogressWait() says, then signals the
// image's render-done in its submit; Qt signals release for the image it stops
// presenting and waits render-done on each image it is handed.
struct SyncedHandoff {
    TestSwapchain                      chain;
    std::array<FakeBinarySemaphore, 3> render_done {
        { { "render-done", 0 }, { "render-done", 1 }, { "render-done", 2 } }
    };
    std::array<FakeBinarySemaphore, 3> release {
        { { "release", 0 }, { "release", 1 }, { "release", 2 } }
    };
    int shown { 0 };
    int newest_shown { 0 };
    int frames { 0 };

    void produce() {
        Image* img = chain.getInprogress();
        switch (chain.inprogressWait()) {
        case Wait::None: break;
        case Wait::Release: release[img->index].wait(); break;
        case Wait::DrainRenderDone: render_done[img->index].wait(); break;
        }
        render_done[img->index].signal();
        img->frame = ++frames;
        chain.renderFrame();
    }

    void repaint() {
        Image* img = chain.eatFrame([this](Image* outgoing) {
            release[outgoing->index].signal();
        });
        if (! img) return;
        render_done[img->index].wait();
        shown = img->frame;
        CHECK_MESSAGE(shown >= newest_shown,
                      "repaint went back from frame ",
                      newest_shown,
                      " to frame ",
                      shown);
        if (shown > newest_shown) newest_shown = shown;
    }

    // 'P' = the render thread publishes a frame, 'E' = Qt repaints.
    void run(const char* script) {
        for (const char* c = script; *c; ++c) {
            if (*c == 'P')
                produce();
            else if (*c == 'E')
                repaint();
        }
    }
};

} // namespace

TEST_SUITE("TripleSwapchain sync") {
    TEST_CASE("the first render into each image waits on nothing") {
        TestSwapchain chain;
        CHECK(chain.inprogressWait() == Wait::None);
        chain.produce(1); // the never-presented initial ready image comes back
        CHECK(chain.inprogressWait() == Wait::None);
    }

    TEST_CASE("an image Qt stopped presenting is waited on for release before reuse") {
        TestSwapchain chain;
        chain.produce(1);
        Image* initially_presented = nullptr;
        Image* shown               = chain.eatFrame([&](Image* outgoing) {
            initially_presented = outgoing;
        });
        REQUIRE(shown != nullptr);
        REQUIRE(initially_presented != nullptr);
        chain.produce(2); // the retired image is the only one free now
        CHECK(chain.getInprogress() == initially_presented);
        CHECK(chain.inprogressWait() == Wait::Release);
    }

    TEST_CASE("a frame dropped before any repaint has its render-done drained before reuse") {
        TestSwapchain chain;
        chain.produce(1);
        Image* dropped = chain.getInprogress(); // gets frame 2, never eaten
        chain.produce(2);
        chain.produce(3); // frame 2 is pushed out of ready unseen
        CHECK(chain.getInprogress() == dropped);
        CHECK(chain.inprogressWait() == Wait::DrainRenderDone);
    }

    TEST_CASE("a render thread Qt never eats from keeps draining, never waits on release") {
        TestSwapchain chain;
        chain.produce(1);
        for (int n = 2; n <= 8; ++n) {
            chain.produce(n);
            CHECK(chain.inprogressWait() == Wait::DrainRenderDone);
        }
    }

    TEST_CASE("a repaint with nothing new retires nothing and hands over nothing") {
        TestSwapchain chain;
        chain.produce(1);
        chain.eatFrame([](Image*) {
        });
        int    retired = 0;
        Image* img     = chain.eatFrame([&](Image*) {
            ++retired;
        });
        CHECK(img == nullptr);
        CHECK(retired == 0);
    }

    // The release signal must be issued before the image can reach the render
    // thread, or the render thread's wait finds no signal pending.  A publish
    // racing the retire must not be able to pick up the image being retired.
    TEST_CASE("the image being retired cannot reach the render thread before its release") {
        TestSwapchain chain;
        chain.produce(1);
        chain.eatFrame([](Image*) {
        });
        chain.produce(2);
        Image* picked_up_during_retire = nullptr;
        Image* retiring                = nullptr;
        chain.eatFrame([&](Image* outgoing) {
            retiring = outgoing;
            chain.produce(3);
            picked_up_during_retire = chain.getInprogress();
        });
        REQUIRE(retiring != nullptr);
        CHECK(picked_up_during_retire != retiring);
    }

    TEST_CASE("semaphores stay balanced and frames stay in order across interleavings") {
        const char* scripts[] = {
            "PEPEPEPEPE",   "PPPPEPPPPE",   "PEEEPEEEPE",   "EEPPEEPPEE",
            "PPEPPEPPEPPE", "PEPPPEEPEPPP", "PPPPPPPPPPEE", "PEPEEPPPEEPE",
        };
        for (const char* script : scripts) {
            CAPTURE(script);
            SyncedHandoff h;
            h.run(script);
            h.run("PEPPEPEE"); // and it keeps going afterwards
        }
    }

    TEST_CASE("semaphores stay balanced when a repaint lands inside a publish") {
        SyncedHandoff h;
        h.run("PE");
        h.produce(); // published, not yet eaten
        h.chain.arm_midpoint([&] {
            h.repaint();
        });
        h.produce();
        h.run("EPEPPE");
        CHECK(h.newest_shown == h.shown);
    }
}
