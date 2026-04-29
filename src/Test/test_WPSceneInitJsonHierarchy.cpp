// Tests that Scene::SerializeLayerInitialStates emits a `pn` (parent name)
// field for layers whose transform parent is a named layer (not the
// scene root, not nullptr).  Other init-state fields are out of scope here.

#include <doctest.h>

#include "Scene/Scene.h"
#include "Scene/SceneNode.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

using nlohmann::json;
using namespace wallpaper;

namespace
{

// Helper: build a tiny scene with parent/child name pairs and return the
// parsed JSON of SerializeLayerInitialStates().  Each entry pair is
// (layer_name, parent_name).  An empty parent_name attaches to scene root.
json buildAndParseInitJson(const std::vector<std::pair<std::string, std::string>>& parents) {
    Scene s;
    // sceneGraph already constructed by Scene::Scene(); we re-use it.
    int next_id = 1;
    // Pass 1: create a SceneNode + LayerInitialState for each entry, but
    // don't wire parents yet.  Cache shared_ptrs so pass 2 can attach them.
    std::unordered_map<std::string, std::shared_ptr<SceneNode>> nodes;
    for (const auto& [name, _parent] : parents) {
        auto n  = std::make_shared<SceneNode>();
        n->ID() = next_id;
        nodes[name] = n;
        s.nodeNameToId[name] = next_id;
        s.nodeById[next_id]  = n.get();
        s.layerInitialStates[name] = Scene::LayerInitialState {};
        ++next_id;
    }
    // Pass 2: attach to either a named parent or the scene root.
    for (const auto& [name, parent_name] : parents) {
        if (parent_name.empty()) {
            s.sceneGraph->AppendChild(nodes[name]);
        } else {
            auto pit = nodes.find(parent_name);
            REQUIRE(pit != nodes.end());
            pit->second->AppendChild(nodes[name]);
        }
    }
    return json::parse(s.SerializeLayerInitialStates());
}

} // namespace

TEST_SUITE("WPSceneInitJson.Hierarchy") {
    TEST_CASE("layer with named parent emits pn field") {
        auto j = buildAndParseInitJson({ { "parentGroup", "" }, { "child", "parentGroup" } });
        REQUIRE(j.contains("child"));
        CHECK(j["child"].contains("pn"));
        CHECK(j["child"]["pn"].get<std::string>() == "parentGroup");
    }

    TEST_CASE("root-child layer omits pn field") {
        auto j = buildAndParseInitJson({ { "rootKid", "" } });
        REQUIRE(j.contains("rootKid"));
        CHECK_FALSE(j["rootKid"].contains("pn"));
    }

    TEST_CASE("orphan layer (no parent at all) omits pn field") {
        // Build a scene with one layer and an explicit nullptr parent (don't
        // attach to sceneGraph).  Constructed manually because the helper
        // always attaches.
        Scene s;
        auto  n = std::make_shared<SceneNode>();
        n->ID() = 1;
        s.nodeNameToId["orphan"] = 1;
        s.nodeById[1]            = n.get();
        s.layerInitialStates["orphan"] = Scene::LayerInitialState {};
        // Deliberately do NOT call sceneGraph->AppendChild(n).
        auto j = json::parse(s.SerializeLayerInitialStates());
        REQUIRE(j.contains("orphan"));
        CHECK_FALSE(j["orphan"].contains("pn"));
    }

    TEST_CASE("emits id field for every named layer") {
        auto j = buildAndParseInitJson({ { "a", "" }, { "b", "a" } });
        CHECK(j["a"]["id"].get<int>() == 1);
        CHECK(j["b"]["id"].get<int>() == 2);
    }
}
