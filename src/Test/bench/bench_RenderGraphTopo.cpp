// Bench: DependencyGraph::TopologicalOrder over a synthetic 30-node DAG.
// Pins the topo-sort hot path used by RenderGraph + SceneToRenderGraph each
// time the graph is rebuilt.  An accidental n^2 sweep (or std::map keyed by
// NodeID rather than the vector-based incidence sets) would balloon this
// bench 5-10x.

#include "nanobench.h"

#include "RenderGraph/DependencyGraph.hpp"

#include <memory>
#include <random>
#include <vector>

void register_RenderGraphTopo(ankerl::nanobench::Bench& bench) {
    using namespace wallpaper::rg;

    // Build the graph once outside the measurement loop -- the topo-sort
    // call is what we want to time, not Connect().
    DependencyGraph g;
    std::vector<NodeID> ids;
    ids.reserve(30);
    for (int i = 0; i < 30; ++i) {
        ids.push_back(g.AddNode(std::make_unique<DependencyGraph::Node>()));
    }
    std::mt19937 rng { 0xDEADBEEFu };
    for (int i = 1; i < 30; ++i) {
        // 0..3 inbound edges per node, all from strictly earlier nodes so
        // the DAG invariant holds.
        const int edges = std::uniform_int_distribution<>(0, 3)(rng);
        for (int d = 0; d < edges; ++d) {
            const int src = std::uniform_int_distribution<>(0, i - 1)(rng);
            g.Connect(ids[src], ids[i]);
        }
    }

    bench.run("RenderGraphTopo", [&] {
        auto order = g.TopologicalOrder();
        ankerl::nanobench::doNotOptimizeAway(order);
    });
}
