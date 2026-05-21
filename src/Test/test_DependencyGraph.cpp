#include <doctest.h>

#include <memory>
#include <vector>
#include "RenderGraph/DependencyGraph.hpp"

namespace rg = wallpaper::rg;

TEST_SUITE("DependencyGraph") {
    TEST_CASE("empty graph: no cycle, empty order") {
        rg::DependencyGraph g;
        CHECK_FALSE(g.HasCycle());
        CHECK(g.TopologicalOrder().empty());
    }

    // Index of a node id within a topo order (-1 = not found).
    static auto orderPos = [](const std::vector<rg::NodeID>& order, rg::NodeID id) -> long {
        for (long i = 0; i < (long)order.size(); ++i)
            if (order[i] == id) return i;
        return -1;
    };

    TEST_CASE("acyclic chain: no cycle, valid topo order a<b<c") {
        rg::DependencyGraph g;
        auto                a = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        auto                b = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        auto                c = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        g.Connect(a, b);
        g.Connect(b, c);
        CHECK_FALSE(g.HasCycle());
        auto order = g.TopologicalOrder();
        REQUIRE(order.size() == 3);
        // out-edge = "runs before": a before b before c.
        CHECK(orderPos(order, a) < orderPos(order, b));
        CHECK(orderPos(order, b) < orderPos(order, c));
    }

    TEST_CASE("simple cycle A->B->A detected") {
        rg::DependencyGraph g;
        auto                a = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        auto                b = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        g.Connect(a, b);
        g.Connect(b, a);
        CHECK(g.HasCycle()); // was: silently false / invalid order
    }

    TEST_CASE("self-loop A->A detected") {
        rg::DependencyGraph g;
        auto                a = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        g.Connect(a, a);
        CHECK(g.HasCycle());
    }

    TEST_CASE("deeper back-edge A->B->C->A detected") {
        rg::DependencyGraph g;
        auto                a = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        auto                b = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        auto                c = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        g.Connect(a, b);
        g.Connect(b, c);
        g.Connect(c, a);
        CHECK(g.HasCycle());
    }

    TEST_CASE("cyclic TopologicalOrder does not crash and stays in bounds") {
        rg::DependencyGraph g;
        auto                a = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        auto                b = g.AddNode(std::make_unique<rg::DependencyGraph::Node>());
        g.Connect(a, b);
        g.Connect(b, a);
        // Logs LOG_ERROR and returns an (invalid) order — must not throw, and
        // every returned id must be a valid node index.
        std::vector<rg::NodeID> order;
        CHECK_NOTHROW(order = g.TopologicalOrder());
        for (auto id : order) CHECK(id < g.NodeNum());
    }
}
