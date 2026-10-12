#pragma once
#include "Core/NoCopyMove.hpp"
#include "Vulkan/StagingBuffer.hpp"
#include <memory>

namespace wallpaper
{
namespace vulkan
{

struct RenderingResources {
    vvk::CommandBuffer command;

    vvk::Fence fence_frame; // used by offscreen path

    StagingBuffer* vertex_buf;
    StagingBuffer* dyn_buf;

    // A GroupBegin that could not build its framebuffer sets this; the
    // members and end of that group skip instead of recording draws outside
    // a render pass.  Cleared at the top of every frame and by the GroupEnd.
    bool group_abandoned { false };
};
} // namespace vulkan
} // namespace wallpaper
