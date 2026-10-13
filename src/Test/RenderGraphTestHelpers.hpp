#pragma once

// Helpers shared by the suites that build a render graph with
// sceneToRenderGraph() and then inspect individual passes.

#include "RenderGraph/RenderGraph.hpp"
#include "Scene/SceneNode.h"
#include "VulkanRender/CustomShaderPass.hpp"

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

} // namespace wek::test
