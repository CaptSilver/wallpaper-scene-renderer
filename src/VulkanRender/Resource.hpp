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
};
} // namespace vulkan
} // namespace wallpaper
