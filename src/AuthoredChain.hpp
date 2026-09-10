#pragma once
#include <unordered_map>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "Core/Literals.hpp"

namespace wallpaper
{

// Accumulate an object's world transform from the authored parent chain.
//
// `authored_local` maps an object id to its declared parent and its own local
// transform. Walking the authored chain rather than the live scene graph
// matters because a node feeding an effect chain has its world node reset to
// identity, so the live chain no longer carries the transform its children
// need.
//
// An id absent from the map contributes identity, and the depth bound keeps a
// malformed scene with a parent cycle from hanging the parser.
inline Eigen::Matrix4d WorldFromAuthoredChain(
    i32 id, const std::unordered_map<i32, std::pair<i32, Eigen::Matrix4d>>& authored_local) {
    constexpr int max_depth = 64;

    std::vector<i32> chain;
    i32              c = id;
    for (int depth = 0; depth < max_depth && c >= 0; ++depth) {
        chain.push_back(c);
        auto it = authored_local.find(c);
        if (it == authored_local.end()) break;
        c = it->second.first;
    }

    Eigen::Matrix4d w = Eigen::Matrix4d::Identity();
    for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
        auto lit = authored_local.find(*rit);
        if (lit != authored_local.end()) w = w * lit->second.second;
    }
    return w;
}

} // namespace wallpaper
