// Tests for ParseGroupNode — the group-object parse helper extracted
// from WPSceneParser.
//
// Coverage:
//   - origin/scale/angles defaults and overrides
//   - id required, parent_id optional
//   - `visible: false` reflected on the SceneNode (the bug this helper
//     was extracted to fix — group `visible:false` previously ignored,
//     letting renderable descendants of e.g. Solar System's `p4cloud`
//     group render despite the hidden parent).
//   - cascade through SceneNode::IsVisible() when a renderable child
//     is parented to a hidden group.

#include <doctest.h>

#include "WPSceneGroupParse.hpp"

#include <nlohmann/json.hpp>

using nlohmann::json;
using namespace wallpaper;

TEST_SUITE("WPSceneGroupParse") {
    TEST_CASE("minimal group: id only, defaults populate the rest") {
        auto j = json::parse(R"({"id": 42})");
        auto g = ParseGroupNode(j);
        CHECK(g.id == 42);
        CHECK(g.parent_id == -1);
        CHECK(g.visible);
        REQUIRE(g.node);
        CHECK(g.node->ID() == 42);
        CHECK(g.node->IsVisible());
        CHECK(g.node->Translate().isApprox(Eigen::Vector3f::Zero()));
        CHECK(g.node->Scale().isApprox(Eigen::Vector3f(1.0f, 1.0f, 1.0f)));
    }

    TEST_CASE("origin/scale/angles applied from JSON") {
        auto j = json::parse(R"({
            "id": 7,
            "origin": "100 200 -50",
            "scale": "2 3 4",
            "angles": "0 0 0"
        })");
        auto g = ParseGroupNode(j);
        CHECK(g.node->Translate().isApprox(Eigen::Vector3f(100.0f, 200.0f, -50.0f)));
        CHECK(g.node->Scale().isApprox(Eigen::Vector3f(2.0f, 3.0f, 4.0f)));
    }

    TEST_CASE("parent field captured into parent_id") {
        auto j = json::parse(R"({"id": 3, "parent": 99})");
        auto g = ParseGroupNode(j);
        CHECK(g.parent_id == 99);
    }

    TEST_CASE("visible:false propagates to SceneNode (the Solar System fix)") {
        auto j = json::parse(R"({"id": 1385, "visible": false})");
        auto g = ParseGroupNode(j);
        CHECK_FALSE(g.visible);
        REQUIRE(g.node);
        CHECK_FALSE(g.node->IsVisible());
    }

    TEST_CASE("visible:true is the default when key absent") {
        auto j = json::parse(R"({"id": 1})");
        auto g = ParseGroupNode(j);
        CHECK(g.visible);
        CHECK(g.node->IsVisible());
    }

    TEST_CASE("hidden group hides renderable descendant via parent-walk") {
        // Matches Solar System pattern: group id=1385 visible:false with
        // a renderable child id=1386 (default-visible).  The child must
        // report IsVisible()=false once parented under the hidden group.
        auto group_json = json::parse(R"({"id": 1385, "visible": false})");
        auto group      = ParseGroupNode(group_json).node;
        REQUIRE(group);
        CHECK_FALSE(group->IsVisible());

        auto child = std::make_shared<SceneNode>();
        child->SetVisible(true);
        CHECK(child->IsVisible());

        group->AppendChild(child);
        CHECK_FALSE(child->IsVisible());

        // Restoring the group's visibility lifts the child too.
        group->SetVisible(true);
        CHECK(child->IsVisible());
    }

    TEST_CASE("missing id falls back to 0 (GET_JSON_NAME_VALUE logs, doesn't throw)") {
        auto j = json::parse(R"({"visible": true})");
        auto g = ParseGroupNode(j);
        CHECK(g.id == 0);
        REQUIRE(g.node);
        CHECK(g.node->ID() == 0);
    }
}
