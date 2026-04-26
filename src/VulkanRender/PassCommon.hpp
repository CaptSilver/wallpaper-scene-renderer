#pragma once
#include "Vulkan/Instance.hpp"
#include "Type.hpp"
#include "Vulkan/TextureCache.hpp"
#include "Scene/SceneRenderTarget.h"
// SetBlend / SetAttachmentLoadOp moved to PassBlendFactors.hpp so unit tests
// in src/Test/ can exercise them without pulling in Instance/TextureCache.
#include "VulkanRender/PassBlendFactors.hpp"

namespace wallpaper
{
namespace vulkan
{

inline TextureKey ToTexKey(wallpaper::SceneRenderTarget rt) {
    return TextureKey {
        .width        = rt.width,
        .height       = rt.height,
        .usage        = {},
        .format       = rt.format,
        .sample       = rt.sample,
        .mipmap_level = rt.mipmap_level,
    };
}
} // namespace vulkan
} // namespace wallpaper
