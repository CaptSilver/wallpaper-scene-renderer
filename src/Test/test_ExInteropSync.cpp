#include <doctest.h>

#include "Swapchain/ExInteropSync.hpp"

using wallpaper::ExInprogressWait;
using wallpaper::InteropSyncEnabled;
using wallpaper::PlanExSubmitSync;

TEST_SUITE("ExInteropSync") {
    TEST_CASE("with sync off a submit waits on and signals nothing") {
        for (auto wait : { ExInprogressWait::None,
                           ExInprogressWait::Release,
                           ExInprogressWait::DrainRenderDone }) {
            auto plan = PlanExSubmitSync(wait, false);
            CHECK_FALSE(plan.wait_release);
            CHECK_FALSE(plan.wait_render_done);
            CHECK_FALSE(plan.signal_render_done);
        }
    }

    TEST_CASE("with sync on every submit signals the image's render-done") {
        for (auto wait : { ExInprogressWait::None,
                           ExInprogressWait::Release,
                           ExInprogressWait::DrainRenderDone }) {
            CHECK(PlanExSubmitSync(wait, true).signal_render_done);
        }
    }

    TEST_CASE("an image that was never presented is rendered without waiting") {
        auto plan = PlanExSubmitSync(ExInprogressWait::None, true);
        CHECK_FALSE(plan.wait_release);
        CHECK_FALSE(plan.wait_render_done);
    }

    TEST_CASE("an image the consumer retired waits for its release only") {
        auto plan = PlanExSubmitSync(ExInprogressWait::Release, true);
        CHECK(plan.wait_release);
        CHECK_FALSE(plan.wait_render_done);
    }

    TEST_CASE("a dropped frame's image consumes its own render-done before signalling it again") {
        auto plan = PlanExSubmitSync(ExInprogressWait::DrainRenderDone, true);
        CHECK(plan.wait_render_done);
        CHECK_FALSE(plan.wait_release);
    }

    TEST_CASE("sync is on when GL can import semaphores and nothing turns it off") {
        CHECK(InteropSyncEnabled(true, nullptr));
        CHECK(InteropSyncEnabled(true, ""));
        CHECK(InteropSyncEnabled(true, "1"));
    }

    TEST_CASE("WEKDE_INTEROP_SYNC=0 turns sync off") { CHECK_FALSE(InteropSyncEnabled(true, "0")); }

    TEST_CASE("sync stays off when GL cannot import semaphores, whatever the variable says") {
        CHECK_FALSE(InteropSyncEnabled(false, nullptr));
        CHECK_FALSE(InteropSyncEnabled(false, "1"));
    }
}
