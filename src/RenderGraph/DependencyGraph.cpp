#include "DependencyGraph.hpp"
#include <algorithm>
#include <functional>
#include <fstream>
#include <cassert>
#include "Utils/Logging.h"

using namespace wallpaper::rg;

std::vector<NodeID> DependencyGraph::GetNodeOut(NodeID i) const {
    const auto&         nexts = m_nodeNext[i];
    std::vector<NodeID> result(nexts.size());
    std::transform(nexts.begin(), nexts.end(), result.begin(), [](NodeID i) {
        return i;
    });
    return result;
}

std::vector<NodeID> DependencyGraph::GetNodeIn(NodeID i) const {
    std::vector<NodeID> result;
    for (NodeID in = 0; in < NodeNum(); in++) {
        for (NodeID j : m_nodeNext[in]) {
            if (i == j) result.push_back(in);
        }
    }
    return result;
}

NodeID DependencyGraph::AddNode(std::unique_ptr<Node>&& node) {
    m_nodes.emplace_back(std::move(node));
    m_nodeNext.push_back({});
    Node& n = *(m_nodes.back());
    n.id    = m_nodes.size() - 1;
    return n.id;
}
void DependencyGraph::Connect(NodeID n1, NodeID n2) { m_nodeNext[n1].insert(n2); }

typedef std::function<std::vector<NodeID>(NodeID)> NextNodeOp;
typedef std::function<void(NodeID)>                DfsCallbackOp;

namespace
{
// Three-colour DFS: White = unvisited, Grey = on the current recursion stack,
// Black = fully explored.  A Grey neighbour is a back-edge — the signature of a
// cycle.  The old visited-only DFS used a single bool and could not tell Grey
// from Black, so it skipped back-edges silently and returned a plausible-but-
// invalid order.  Returns false iff a cycle was found.  `post` (if set) runs in
// post-order (for the reversed topo sort).
enum class Color : uint8_t
{
    White,
    Grey,
    Black
};

bool DfsVisit(NodeID id, std::vector<Color>& color, const NextNodeOp& next,
              const DfsCallbackOp& post) {
    color[id]    = Color::Grey; // on current path
    bool acyclic = true;
    for (NodeID nxt : next(id)) {
        if (color[nxt] == Color::Grey) { // back-edge → cycle
            LOG_ERROR("render graph cycle: edge %zu -> %zu", id, nxt);
            acyclic = false;
        } else if (color[nxt] == Color::White) {
            acyclic = DfsVisit(nxt, color, next, post) && acyclic;
        }
        // Black: cross/forward edge — fine, already fully explored.
    }
    color[id] = Color::Black;
    if (post) post(id);
    return acyclic;
}
} // namespace

bool DependencyGraph::HasCycle() const {
    std::vector<Color> color(m_nodes.size(), Color::White);
    auto               nextOut = [this](NodeID i) {
        return GetNodeOut(i);
    };
    for (usize i = 0; i < color.size(); i++) {
        if (color[i] == Color::White && ! DfsVisit(i, color, nextOut, DfsCallbackOp())) return true;
    }
    return false;
}

std::vector<NodeID> DependencyGraph::TopologicalOrder() const {
    std::vector<NodeID> result;
    result.reserve(m_nodes.size());
    std::vector<Color> color(m_nodes.size(), Color::White);
    auto               nextOut = [this](NodeID i) {
        return GetNodeOut(i);
    };
    bool acyclic = true;
    for (usize i = 0; i < color.size(); i++) {
        if (color[i] == Color::White)
            acyclic = DfsVisit(i,
                               color,
                               nextOut,
                               [&](NodeID n) {
                                   result.push_back(n);
                               }) &&
                      acyclic;
    }
    if (! acyclic) LOG_ERROR("TopologicalOrder on cyclic render graph — ordering is invalid");
    std::reverse(result.begin(), result.end());
    return result;
}

void DependencyGraph::ToGraphviz(std::string_view path) const {
    std::string output;
    output.reserve(4096);
    std::ofstream fs;
    fs.open(std::string(path), std::fstream::out | std::fstream::trunc);
    if (! fs.is_open()) return;

    output += R"(
digraph framegraph {
node [shape=box]
)";
    for (const auto& n : m_nodes) {
        output += n->ToGraphviz();
        output += '\n';
    }
    for (usize i = 0; i < m_nodeNext.size(); i++) {
        for (const auto& e : m_nodeNext[i]) {
            output += "n" + std::to_string(i) + "->n" + std::to_string(e) + "\n";
        }
    }

    output += "}";

    fs << output;
    fs.close();
};
