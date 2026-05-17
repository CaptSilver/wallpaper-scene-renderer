#include <doctest.h>

#include "SpriteAnimation.hpp"

using namespace wallpaper;

TEST_SUITE("SpriteAnimation") {
    TEST_CASE("Empty animation has zero frames") {
        SpriteAnimation anim;
        CHECK(anim.numFrames() == 0);
    }

    TEST_CASE("Single frame") {
        SpriteAnimation anim;
        SpriteFrame     f;
        f.imageId   = 0;
        f.frametime = 1.0f;
        f.x         = 0.0f;
        f.y         = 0.0f;
        f.width     = 0.5f;
        f.height    = 0.5f;
        anim.AppendFrame(f);

        CHECK(anim.numFrames() == 1);

        auto& cur = anim.GetCurFrame();
        CHECK(cur.imageId == 0);
        CHECK(cur.frametime == doctest::Approx(1.0f));
        CHECK(cur.width == doctest::Approx(0.5f));
    }

    TEST_CASE("GetAnimateFrame cycles through frames") {
        SpriteAnimation anim;

        SpriteFrame f0;
        f0.imageId   = 0;
        f0.frametime = 0.5f;
        anim.AppendFrame(f0);

        SpriteFrame f1;
        f1.imageId   = 1;
        f1.frametime = 0.5f;
        anim.AppendFrame(f1);

        CHECK(anim.numFrames() == 2);

        // First call: m_remainTime starts at 0, 0 - 0.0 = 0 which is NOT < 0,
        // so no switch happens on a zero-time call
        {
            auto& frame = anim.GetAnimateFrame(0.0);
            CHECK(frame.imageId == 0);
        }

        // Advance by 0.6 — exceeds frametime of 0.5, should switch to frame 1
        {
            auto& frame = anim.GetAnimateFrame(0.6);
            CHECK(frame.imageId == 1);
        }

        // Advance by 0.6 again — should wrap back to frame 0
        {
            auto& frame = anim.GetAnimateFrame(0.6);
            CHECK(frame.imageId == 0);
        }
    }

    TEST_CASE("GetAnimateFrame with small time steps stays on frame") {
        SpriteAnimation anim;

        SpriteFrame f0;
        f0.imageId   = 0;
        f0.frametime = 1.0f;
        anim.AppendFrame(f0);

        SpriteFrame f1;
        f1.imageId   = 1;
        f1.frametime = 1.0f;
        anim.AppendFrame(f1);

        // m_remainTime starts at 0, so the first positive-time call triggers a
        // switch (0 - dt < 0). After the switch, m_remainTime is set to the
        // new frame's frametime. Use a tiny dt to trigger the initial switch.
        {
            auto& frame = anim.GetAnimateFrame(0.001);
            // Switched from frame 0 to frame 1, m_remainTime = 1.0
            CHECK(frame.imageId == 1);
        }

        // Small steps (total 0.5s) should stay on frame 1 (frametime = 1.0)
        for (int i = 0; i < 5; i++) {
            auto& frame = anim.GetAnimateFrame(0.1);
            CHECK(frame.imageId == 1);
        }

        // Advance past the remaining ~0.5s to trigger switch back to frame 0
        auto& frame = anim.GetAnimateFrame(0.6);
        CHECK(frame.imageId == 0);
    }

    TEST_CASE("AppendFrame preserves insertion order") {
        SpriteAnimation anim;

        for (int i = 0; i < 5; i++) {
            SpriteFrame f;
            f.imageId   = i;
            f.frametime = 0.1f;
            anim.AppendFrame(f);
        }

        CHECK(anim.numFrames() == 5);
        CHECK(anim.GetCurFrame().imageId == 0);
    }

    // Manual-frame pin: SceneScript writes thisLayer.getTextureAnimation()
    // .setFrame(N) end up here via SetManualFrame.  Game of Life
    // (3453251764) tinted-button sheets cycled through 3 frames at the
    // texture's authored rate ("constantly flashing") until this landed.
    TEST_CASE("SetManualFrame pins frame index against auto-advance time") {
        SpriteAnimation anim;
        for (int i = 0; i < 3; i++) {
            SpriteFrame f;
            f.imageId   = i;
            f.frametime = 0.1f;
            anim.AppendFrame(f);
        }
        CHECK_FALSE(anim.isManualFrame());
        anim.SetManualFrame(2);
        CHECK(anim.isManualFrame());
        CHECK(anim.curFrameIndex() == 2);
        // Even after a long elapsed time, the pinned frame stays.
        for (int i = 0; i < 10; i++) {
            const auto& f = anim.GetAnimateFrame(1.0);
            CHECK(f.imageId == 2);
        }
    }

    TEST_CASE("SetManualFrame clamps out-of-range indices") {
        SpriteAnimation anim;
        for (int i = 0; i < 3; i++) {
            SpriteFrame f;
            f.imageId   = i;
            f.frametime = 0.1f;
            anim.AppendFrame(f);
        }
        anim.SetManualFrame(99);
        CHECK(anim.curFrameIndex() == 2);
        anim.SetManualFrame(-5);
        CHECK(anim.curFrameIndex() == 0);
    }

    TEST_CASE("ClearManualFrame restores time-driven auto-advance") {
        SpriteAnimation anim;
        for (int i = 0; i < 3; i++) {
            SpriteFrame f;
            f.imageId   = i;
            f.frametime = 0.1f;
            anim.AppendFrame(f);
        }
        anim.SetManualFrame(1);
        CHECK(anim.isManualFrame());
        anim.ClearManualFrame();
        CHECK_FALSE(anim.isManualFrame());
        // After clearing, frametime budgeting resumes — single 0.2s tick
        // advances past the budget into the next frame.
        const auto& f = anim.GetAnimateFrame(0.2);
        CHECK(f.imageId == 2);
    }

    TEST_CASE("SetManualFrame on an empty animation is a safe no-op") {
        SpriteAnimation anim;
        anim.SetManualFrame(5);
        CHECK(anim.isManualFrame());
        CHECK(anim.curFrameIndex() == 0);
        CHECK(anim.numFrames() == 0);
    }

    // SceneScript thisLayer.getTextureAnimation().duration reads this — the
    // Rella firework script (3363252053) computes `ani.duration * 1000 /
    // ani.frameCount` to wait one frame before pausing.  Without the sum the
    // value was undefined, the wait timer fired NaN, and the inner pause/play
    // chain ran every tick — the JS proxy used to hardcode frameCount=1 so the
    // end-of-cycle branch fired immediately on every update().
    TEST_CASE("totalDuration sums every frame's frametime in seconds") {
        SpriteAnimation anim;
        CHECK(anim.totalDuration() == 0.0f); // empty animation
        SpriteFrame f1;
        f1.frametime = 0.1f;
        SpriteFrame f2;
        f2.frametime = 0.25f;
        SpriteFrame f3;
        f3.frametime = 0.05f;
        anim.AppendFrame(f1);
        anim.AppendFrame(f2);
        anim.AppendFrame(f3);
        CHECK(anim.totalDuration() == doctest::Approx(0.4f));
    }

    TEST_CASE("totalDuration handles many uniform frames (Rella firework "
              "≈151 frames @ 30ms)") {
        SpriteAnimation anim;
        for (int i = 0; i < 151; i++) {
            SpriteFrame f;
            f.frametime = 0.03f;
            anim.AppendFrame(f);
        }
        CHECK(anim.numFrames() == 151);
        // 151 * 0.03 = 4.53 seconds — matches the live read-back we observed
        // for `materials/合成 1_00000` on wallpaper 3363252053.
        CHECK(anim.totalDuration() == doctest::Approx(4.53f).epsilon(0.001f));
    }

} // TEST_SUITE
