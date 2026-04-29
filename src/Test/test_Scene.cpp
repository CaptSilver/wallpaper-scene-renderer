#include <doctest.h>

#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneNode.h"

#include <memory>
#include <utility>

using namespace wallpaper;

TEST_SUITE("Scene") {
    TEST_CASE("PassFrameTime accumulates elapsingTime and sets frameTime") {
        Scene s;
        CHECK(s.frameTime == doctest::Approx(0.0));
        CHECK(s.elapsingTime == doctest::Approx(0.0));
        s.PassFrameTime(0.016);
        CHECK(s.frameTime == doctest::Approx(0.016));
        CHECK(s.elapsingTime == doctest::Approx(0.016));
        s.PassFrameTime(0.032);
        CHECK(s.frameTime == doctest::Approx(0.032));
        CHECK(s.elapsingTime == doctest::Approx(0.048));
    }

    TEST_CASE("UpdateLinkedCamera with no-match name is a no-op") {
        Scene s;
        s.UpdateLinkedCamera("missing"); // neither linkedCameras nor cameras populated
        CHECK(true);                     // no crash
    }

    TEST_CASE("UpdateLinkedCamera clones the source camera's width/height to followers") {
        Scene s;

        auto src          = std::make_shared<SceneCamera>(1920, 1080, 0.1f, 100.0f);
        s.cameras["main"] = src;

        auto f1                 = std::make_shared<SceneCamera>(640, 480, 0.1f, 100.0f);
        auto f2                 = std::make_shared<SceneCamera>(320, 240, 0.1f, 100.0f);
        s.cameras["follower1"]  = f1;
        s.cameras["follower2"]  = f2;
        s.linkedCameras["main"] = { "follower1", "follower2", "missing_follower" };

        s.UpdateLinkedCamera("main");
        // Clone() copies width/height; followers now match the source.
        CHECK(f1->Width() == doctest::Approx(1920.0));
        CHECK(f1->Height() == doctest::Approx(1080.0));
        CHECK(f2->Width() == doctest::Approx(1920.0));
        CHECK(f2->Height() == doctest::Approx(1080.0));
    }

} // Scene

// ===========================================================================
// Pending-parent-change queue + ApplyPendingParentChanges drain
// ===========================================================================

TEST_SUITE("Scene.PendingParentChanges") {
    TEST_CASE("queue stores child→parent pairs in FIFO order") {
        Scene s;
        s.QueueParentChange(10, 20);
        s.QueueParentChange(11, -1);
        s.QueueParentChange(12, 20);
        auto pending = s.TakePendingParentChanges();
        REQUIRE(pending.size() == 3);
        CHECK(pending[0] == std::pair<i32, i32> { 10, 20 });
        CHECK(pending[1] == std::pair<i32, i32> { 11, -1 });
        CHECK(pending[2] == std::pair<i32, i32> { 12, 20 });
    }

    TEST_CASE("Take leaves the queue empty for next caller") {
        Scene s;
        s.QueueParentChange(1, 2);
        (void)s.TakePendingParentChanges();
        auto pending = s.TakePendingParentChanges();
        CHECK(pending.empty());
    }

    TEST_CASE("ApplyPendingParentChanges reparents a child to a new parent") {
        Scene s;
        auto  a   = std::make_shared<SceneNode>();
        auto  b   = std::make_shared<SceneNode>();
        auto  c   = std::make_shared<SceneNode>();
        a->ID()   = 1;
        b->ID()   = 2;
        c->ID()   = 3;
        s.sceneGraph = std::make_shared<SceneNode>();
        s.sceneGraph->AppendChild(a);
        s.sceneGraph->AppendChild(b);
        a->AppendChild(c);
        s.nodeById[1] = a.get();
        s.nodeById[2] = b.get();
        s.nodeById[3] = c.get();

        s.QueueParentChange(3, 2); // reparent c from a → b
        s.ApplyPendingParentChanges();

        CHECK(c->Parent() == b.get());
        CHECK(a->GetChildren().empty());
        CHECK(b->GetChildren().size() == 1);
        CHECK(b->GetChildren().front().get() == c.get());
    }

    TEST_CASE("ApplyPendingParentChanges with parent_id=-1 reattaches to sceneGraph") {
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto p       = std::make_shared<SceneNode>();
        auto c       = std::make_shared<SceneNode>();
        p->ID()      = 1;
        c->ID()      = 2;
        s.sceneGraph->AppendChild(p);
        p->AppendChild(c);
        s.nodeById[1] = p.get();
        s.nodeById[2] = c.get();

        s.QueueParentChange(2, -1);
        s.ApplyPendingParentChanges();

        CHECK(c->Parent() == s.sceneGraph.get());
        CHECK(p->GetChildren().empty());
    }

    TEST_CASE("ApplyPendingParentChanges skips unknown child id") {
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        s.QueueParentChange(999, -1); // unknown id
        CHECK_NOTHROW(s.ApplyPendingParentChanges());
    }

    TEST_CASE("ApplyPendingParentChanges skips unknown parent id (non-root)") {
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto c       = std::make_shared<SceneNode>();
        c->ID()      = 1;
        s.sceneGraph->AppendChild(c);
        s.nodeById[1] = c.get();

        s.QueueParentChange(1, 999);
        s.ApplyPendingParentChanges();
        // No-op: c stays under sceneGraph
        CHECK(c->Parent() == s.sceneGraph.get());
    }

} // Scene.PendingParentChanges
