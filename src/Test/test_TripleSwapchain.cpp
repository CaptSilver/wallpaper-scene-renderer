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
};

// TripleSwapchain over three plain images.  The render thread and the Qt
// render thread race on these slots for real; here the test plays both sides
// on one thread and uses `on_publish_midpoint` to run the consumer at a chosen
// point inside renderFrame(), which is how a preempted render thread would
// interleave with eatFrame().
class TestSwapchain : public TripleSwapchain<Image> {
public:
    TestSwapchain() {
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
struct Screen {
    TestSwapchain& chain;
    int            shown { 0 };
    int            newest_shown { 0 };

    void repaint() {
        if (Image* img = chain.eatFrame()) {
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
