#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wallpaper
{

// Per-frame attachment-proxy refresh recomposes each attached child's world
// from its parent's CURRENT world.  When attachments are nested (a puppet head
// whose child composite layer has its own attached children), a child must be
// refreshed only AFTER its parent, so the parent's world is already current
// when the child reads it.
//
// Each link is identified by its child id and the id of the parent it tracks.
// A link's "depth" is the number of ancestors — following parent ids — that are
// themselves children in the same set.  Refreshing links in ascending depth
// order guarantees parents-before-children.  Roots (whose parent is not another
// tracked child) get depth 0.
//
// The `seen` guard makes a malformed cyclic parent chain terminate instead of
// looping forever (it just stops counting once it revisits an id).
//
// Pure function — no scene state — so the ordering rule is unit-testable.
inline std::vector<int> attachmentLinkDepths(const std::vector<int>& childIds,
                                             const std::vector<int>& parentIds) {
    std::unordered_map<int, int> childToParent;
    const std::size_t n = childIds.size() < parentIds.size() ? childIds.size() : parentIds.size();
    for (std::size_t i = 0; i < n; ++i) childToParent[childIds[i]] = parentIds[i];

    std::vector<int> depths(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        int                     d = 0;
        int                     p = parentIds[i];
        std::unordered_set<int> seen;
        while (childToParent.count(p) && seen.insert(p).second) {
            ++d;
            p = childToParent[p];
        }
        depths[i] = d;
    }
    return depths;
}

} // namespace wallpaper
