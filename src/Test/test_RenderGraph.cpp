#include <doctest.h>

#include "RenderGraph/DependencyGraph.hpp"
#include "RenderGraph/RenderGraph.hpp"
#include "RenderGraph/Pass.hpp" // VirtualPass — concrete, device-free
#include "RenderGraph/TexNode.hpp"
#include "RenderGraph/PassNode.hpp"

#include <algorithm>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::rg;

namespace
{
std::unique_ptr<DependencyGraph::Node> mkNode() {
    return std::make_unique<DependencyGraph::Node>();
}
long indexOf(const std::vector<NodeID>& v, NodeID x) {
    auto it = std::find(v.begin(), v.end(), x);
    return it == v.end() ? -1 : (long)(it - v.begin());
}
} // namespace

// CPU-only coverage for the pure-data render-graph layer (zero tests before
// Spec 19/27).  DependencyGraph cycle detection is covered separately in
// test_DependencyGraph.cpp; this file adds DAG ordering / in-out symmetry and
// the RenderGraph builder's pass-alias resolution (the alias-aware pass cache
// depends on it).  SceneToRenderGraph is Vulkan-coupled and out of scope.
TEST_SUITE("RenderGraph_DependencyGraph") {
    TEST_CASE("TopologicalOrder: every edge places producer before consumer") {
        // Diamond: a -> b -> d, a -> c -> d.
        DependencyGraph g;
        NodeID          a = g.AddNode(mkNode());
        NodeID          b = g.AddNode(mkNode());
        NodeID          c = g.AddNode(mkNode());
        NodeID          d = g.AddNode(mkNode());
        g.Connect(a, b);
        g.Connect(a, c);
        g.Connect(b, d);
        g.Connect(c, d);

        auto order = g.TopologicalOrder();
        REQUIRE(order.size() == 4);
        const std::vector<std::pair<NodeID, NodeID>> edges {
            { a, b }, { a, c }, { b, d }, { c, d }
        };
        for (auto [u, v] : edges) {
            CHECK(indexOf(order, u) >= 0);
            CHECK(indexOf(order, v) >= 0);
            CHECK(indexOf(order, u) < indexOf(order, v)); // producer before consumer
        }
    }

    TEST_CASE("GetNodeIn/Out symmetry + EdgeNum counts") {
        DependencyGraph g;
        NodeID          a = g.AddNode(mkNode());
        NodeID          b = g.AddNode(mkNode());
        CHECK(g.NodeNum() == 2);
        CHECK(g.EdgeNum() == 0);
        g.Connect(a, b);
        CHECK(g.EdgeNum() == 1);

        CHECK(indexOf(g.GetNodeOut(a), b) >= 0); // b in out(a)
        CHECK(indexOf(g.GetNodeIn(b), a) >= 0);  // a in in(b)
        CHECK(g.GetNodeOut(b).empty());          // no phantom edges
        CHECK(g.GetNodeIn(a).empty());
    }
}

TEST_SUITE("RenderGraph_PassAlias") {
    TEST_CASE("getLastReadTexs attributes a read tex to the reading pass") {
        // passA writes texT; passB reads texT. getLastReadTexs must place texT in
        // passB's last-read set (the alias-aware pass cache keys on exactly this).
        RenderGraph rg;
        PassNode*   pa = rg.addPass<VirtualPass>(
            "passA", PassNode::Type::CustomShader, [](RenderGraphBuilder& b, VirtualPass::Desc&) {
                TexNode* t =
                    b.createTexNode(TexNode::Desc { "T", "key_T", TexNode::TexType::Temp }, true);
                b.write(t);
            });
        PassNode* pb = rg.addPass<VirtualPass>(
            "passB", PassNode::Type::CustomShader, [](RenderGraphBuilder& b, VirtualPass::Desc&) {
                TexNode* t =
                    b.createTexNode(TexNode::Desc { "T", "key_T", TexNode::TexType::Temp }, false);
                b.read(t);
            });
        REQUIRE(pa != nullptr);
        REQUIRE(pb != nullptr);

        auto order = rg.topologicalOrder();
        auto idx   = [&](NodeID id) {
            return (long)(std::find(order.begin(), order.end(), id) - order.begin());
        };
        CHECK(idx(pa->ID()) < idx(pb->ID())); // producer before consumer

        auto lastReads = rg.getLastReadTexs(order);
        REQUIRE(lastReads.size() == order.size()); // per-order-slot indexing
        const long pbSlot = idx(pb->ID());
        REQUIRE(pbSlot >= 0);
        bool sawKeyT = false;
        for (auto* tex : lastReads[(size_t)pbSlot])
            if (tex != nullptr && tex->key() == "key_T") sawKeyT = true;
        CHECK(sawKeyT);
    }

    TEST_CASE("writing a new version after a reader chains the dependency") {
        // passA writes T; passB reads T; passC writes T again (new version).
        // The version-bump dependency must order passC after the reader passB.
        RenderGraph rg;
        PassNode*   pa = rg.addPass<VirtualPass>(
            "passA", PassNode::Type::CustomShader, [](RenderGraphBuilder& b, VirtualPass::Desc&) {
                b.write(
                    b.createTexNode(TexNode::Desc { "T", "key_T", TexNode::TexType::Temp }, true));
            });
        PassNode* pb = rg.addPass<VirtualPass>(
            "passB", PassNode::Type::CustomShader, [](RenderGraphBuilder& b, VirtualPass::Desc&) {
                b.read(
                    b.createTexNode(TexNode::Desc { "T", "key_T", TexNode::TexType::Temp }, false));
            });
        PassNode* pc = rg.addPass<VirtualPass>(
            "passC", PassNode::Type::CustomShader, [](RenderGraphBuilder& b, VirtualPass::Desc&) {
                b.write(
                    b.createTexNode(TexNode::Desc { "T", "key_T", TexNode::TexType::Temp }, true));
            });
        REQUIRE(pa != nullptr);
        REQUIRE(pb != nullptr);
        REQUIRE(pc != nullptr);

        auto order = rg.topologicalOrder();
        auto idx   = [&](NodeID id) {
            return (long)(std::find(order.begin(), order.end(), id) - order.begin());
        };
        CHECK(idx(pa->ID()) < idx(pb->ID())); // reader after writer
        CHECK(idx(pb->ID()) < idx(pc->ID())); // new-version writer after reader
    }
}
