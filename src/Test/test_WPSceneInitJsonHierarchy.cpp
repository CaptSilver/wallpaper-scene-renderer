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

    // A text layer's static text must travel into the init state so a script
    // can read ANOTHER layer's text (thisScene.getLayer(name).text) before
    // that layer ever runs a script.  Wallpaper 3122339805's world clock
    // reads a "Time Difference" layer's "+1"; without the seed it read "" and
    // the clock computed NaN -> "aN:aN:aN".
    TEST_CASE("text layer emits its static text in the init state") {
        Scene s;
        auto  n = std::make_shared<SceneNode>();
        n->ID() = 42;
        s.nodeNameToId["Time Difference 1"] = 42;
        s.nodeById[42]                      = n.get();
        s.layerInitialStates["Time Difference 1"] = Scene::LayerInitialState {};
        s.sceneGraph->AppendChild(n);
        TextLayerInfo tl;
        tl.id          = 42;
        tl.currentText = "+1";
        s.textLayers.push_back(tl);

        auto j = json::parse(s.SerializeLayerInitialStates());
        REQUIRE(j.contains("Time Difference 1"));
        REQUIRE(j["Time Difference 1"].contains("text"));
        CHECK(j["Time Difference 1"]["text"].get<std::string>() == "+1");
    }

    TEST_CASE("text layer with empty text omits the text field") {
        // An empty seed is identical to the proxy's '' default, so don't bloat
        // the JSON with it (matches the halign/valign/font non-empty gating).
        Scene s;
        auto  n = std::make_shared<SceneNode>();
        n->ID() = 43;
        s.nodeNameToId["Blank"] = 43;
        s.nodeById[43]          = n.get();
        s.layerInitialStates["Blank"] = Scene::LayerInitialState {};
        s.sceneGraph->AppendChild(n);
        TextLayerInfo tl;
        tl.id          = 43;
        tl.currentText = "";
        s.textLayers.push_back(tl);

        auto j = json::parse(s.SerializeLayerInitialStates());
        REQUIRE(j.contains("Blank"));
        CHECK_FALSE(j["Blank"].contains("text"));
    }

    TEST_CASE("jsonParentId resolves pn when runtime SceneNode hierarchy is rewired") {
        // Regression for Floating Cat (3367988661): `Background` and
        // `Settings Container` declared `parent: 169` (Text Container) in
        // scene.json, but effect-RT/composelayer node rewiring moved their
        // runtime SceneNode parents up to the scene root.  Pre-fix, the
        // serializer walked SceneNode::Parent() up to the root and never
        // emitted `pn`, so SceneScript's `parent.scale.x` accesses crashed
        // with "Cannot read property 'scale' of null".  Post-fix, the
        // JSON-authored parent ID wins over the live SceneNode hierarchy.
        Scene s;
        // Build three nodes; "parent" is at the scene root, "child" lives
        // under an unnamed intermediate (mimicking effect-RT wrapper).
        auto unnamed_intermediate = std::make_shared<SceneNode>();
        auto parent_node          = std::make_shared<SceneNode>();
        auto child_node           = std::make_shared<SceneNode>();
        parent_node->ID() = 100;
        child_node->ID()  = 200;
        s.nodeNameToId["parent"] = 100;
        s.nodeNameToId["child"]  = 200;
        s.nodeById[100]          = parent_node.get();
        s.nodeById[200]          = child_node.get();
        s.layerInitialStates["parent"] = Scene::LayerInitialState {};
        s.layerInitialStates["child"]  = Scene::LayerInitialState {};
        // Live hierarchy: child → unnamed_intermediate → sceneGraph.  Parent
        // sits separately at the scene root with no live connection to child.
        s.sceneGraph->AppendChild(parent_node);
        s.sceneGraph->AppendChild(unnamed_intermediate);
        unnamed_intermediate->AppendChild(child_node);
        // But the JSON authored child's parent as 100 (parent).
        s.jsonParentId[200] = 100;

        auto j = json::parse(s.SerializeLayerInitialStates());
        REQUIRE(j.contains("child"));
        REQUIRE(j["child"].contains("pn"));
        CHECK(j["child"]["pn"].get<std::string>() == "parent");
    }

    TEST_CASE("SceneNode walk fallback finds named parent through unnamed intermediates") {
        // When the JSON didn't declare a parent (dynamic-pool / script-
        // created layer), the serializer falls back to walking SceneNode::
        // Parent() upward, skipping unnamed intermediates to find the
        // nearest named ancestor.
        Scene s;
        auto wrapper      = std::make_shared<SceneNode>();
        auto named_parent = std::make_shared<SceneNode>();
        auto child        = std::make_shared<SceneNode>();
        named_parent->ID() = 11;
        child->ID()        = 22;
        s.nodeNameToId["namedParent"] = 11;
        s.nodeNameToId["child"]       = 22;
        s.nodeById[11]                = named_parent.get();
        s.nodeById[22]                = child.get();
        s.layerInitialStates["namedParent"] = Scene::LayerInitialState {};
        s.layerInitialStates["child"]       = Scene::LayerInitialState {};
        // child → wrapper → named_parent → sceneGraph
        s.sceneGraph->AppendChild(named_parent);
        named_parent->AppendChild(wrapper);
        wrapper->AppendChild(child);
        // No jsonParentId entry — fallback must walk past `wrapper`.

        auto j = json::parse(s.SerializeLayerInitialStates());
        REQUIRE(j.contains("child"));
        REQUIRE(j["child"].contains("pn"));
        CHECK(j["child"]["pn"].get<std::string>() == "namedParent");
    }
}
