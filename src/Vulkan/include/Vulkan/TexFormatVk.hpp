#pragma once
#include <cassert>
#include <vulkan/vulkan.h>

#include "Type.hpp"

// TextureFormat -> VkFormat mapping.  Lives in its own header (rather than
// inline in TextureCache.cpp) so the unit tests under src/Test can pin it
// without pulling in the rest of the Vulkan device/renderer — mirrors
// VulkanRender/PassBlendFactors.hpp.
namespace wallpaper
{
namespace vulkan
{
inline VkFormat ToVkType(TextureFormat tf) {
    switch (tf) {
    // BC1 (DXT1) carries optional 1-bit punch-through alpha: blocks encoded
    // with color0 <= color1 use the 3-colour mode where palette index 3 is
    // transparent black (RGB=0, A=0).  The _RGB_ Vulkan variant ignores that
    // bit and forces A=1, turning a sprite's intended-transparent surround
    // into OPAQUE BLACK — which then composites as black holes over the
    // background (Painting the Sharks 2468489223: pez4 / 2fish BC1 fish
    // layers punched black rectangles into the water).  The _RGBA_ variant
    // honours the bit; fully-opaque BC1 (all color0 > color1 blocks) decodes
    // identically, so this is strictly more correct and matches WE.
    case TextureFormat::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case TextureFormat::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case TextureFormat::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case TextureFormat::BC7: return VK_FORMAT_BC7_UNORM_BLOCK;
    case TextureFormat::R8: return VK_FORMAT_R8_UNORM;
    case TextureFormat::RG8: return VK_FORMAT_R8G8_UNORM;
    case TextureFormat::RGB8: return VK_FORMAT_R8G8B8_UNORM;
    case TextureFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case TextureFormat::R16F: return VK_FORMAT_R16_SFLOAT;
    case TextureFormat::R32F: return VK_FORMAT_R32_SFLOAT;
    case TextureFormat::BC6H: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case TextureFormat::RGB565: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case TextureFormat::RGBA1010102: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    default: assert(false); return VK_FORMAT_R8G8B8A8_UNORM;
    }
}
} // namespace vulkan
} // namespace wallpaper
