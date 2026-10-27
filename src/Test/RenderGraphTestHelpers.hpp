#pragma once

// Helpers shared by the suites that build a render graph with
// sceneToRenderGraph() and then inspect individual passes.

#include "RenderGraph/RenderGraph.hpp"
#include "Scene/SceneNode.h"
#include "VulkanRender/CopyPass.hpp"
#include "VulkanRender/CustomShaderPass.hpp"

#include <string_view>

namespace wek::test
{

// The CustomShaderPass that draws `node`, or nullptr when the graph has none.
inline wallpaper::vulkan::CustomShaderPass* findPassForNode(wallpaper::rg::RenderGraph& rgraph,
                                                            wallpaper::SceneNode*       node) {
    for (auto id : rgraph.topologicalOrder()) {
        auto* p = dynamic_cast<wallpaper::vulkan::CustomShaderPass*>(rgraph.getPass(id));
        if (p != nullptr && p->desc().node == node) return p;
    }
    return nullptr;
}

// The CopyPass whose destination is `dst`, or nullptr when the graph has none.
inline wallpaper::vulkan::CopyPass* findCopyPassTo(wallpaper::rg::RenderGraph& rgraph,
                                                   std::string_view            dst) {
    for (auto id : rgraph.topologicalOrder()) {
        auto* p = dynamic_cast<wallpaper::vulkan::CopyPass*>(rgraph.getPass(id));
        if (p != nullptr && p->desc().dst == dst) return p;
    }
    return nullptr;
}

// Position of `pass` in the graph's topological order, or -1 when absent.
inline int topoIndexOf(wallpaper::rg::RenderGraph& rgraph, const wallpaper::rg::Pass* pass) {
    int i = 0;
    for (auto id : rgraph.topologicalOrder()) {
        if (rgraph.getPass(id) == pass) return i;
        i++;
    }
    return -1;
}

} // namespace wek::test
