// Pin the TextureFormat -> VkFormat mapping.
//
// Regression guard for the BC1 1-bit-alpha bug: BC1 (DXT1) sprites carry
// optional punch-through alpha (3-colour mode, palette index 3 = transparent).
// Mapping BC1 to the _RGB_ Vulkan variant ignores that bit and forces alpha=1,
// turning a sprite's transparent surround into opaque black holes (seen on
// "Painting the Sharks" 2468489223 pez4 / 2fish layers).  The fix maps BC1 to
// the _RGBA_ variant.

#include <doctest.h>

#include "Type.hpp"
#include "Vulkan/TexFormatVk.hpp"

using wallpaper::TextureFormat;
using wallpaper::vulkan::ToVkType;

TEST_SUITE("ToVkType(TextureFormat)") {
    // The regression that motivated extracting this mapping into a header.
    TEST_CASE("BC1 maps to the RGBA (1-bit alpha) variant, not RGB") {
        CHECK(ToVkType(TextureFormat::BC1) == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
        CHECK(ToVkType(TextureFormat::BC1) != VK_FORMAT_BC1_RGB_UNORM_BLOCK);
    }
    TEST_CASE("BC2/BC3 keep their alpha-bearing block formats") {
        CHECK(ToVkType(TextureFormat::BC2) == VK_FORMAT_BC2_UNORM_BLOCK);
        CHECK(ToVkType(TextureFormat::BC3) == VK_FORMAT_BC3_UNORM_BLOCK);
    }
    TEST_CASE("BC7 / BC6H block formats") {
        CHECK(ToVkType(TextureFormat::BC7) == VK_FORMAT_BC7_UNORM_BLOCK);
        CHECK(ToVkType(TextureFormat::BC6H) == VK_FORMAT_BC6H_UFLOAT_BLOCK);
    }
    TEST_CASE("uncompressed integer formats") {
        CHECK(ToVkType(TextureFormat::R8) == VK_FORMAT_R8_UNORM);
        CHECK(ToVkType(TextureFormat::RG8) == VK_FORMAT_R8G8_UNORM);
        CHECK(ToVkType(TextureFormat::RGB8) == VK_FORMAT_R8G8B8_UNORM);
        CHECK(ToVkType(TextureFormat::RGBA8) == VK_FORMAT_R8G8B8A8_UNORM);
    }
    TEST_CASE("float + packed formats") {
        CHECK(ToVkType(TextureFormat::RGBA16F) == VK_FORMAT_R16G16B16A16_SFLOAT);
        CHECK(ToVkType(TextureFormat::RG16F) == VK_FORMAT_R16G16_SFLOAT);
        CHECK(ToVkType(TextureFormat::R16F) == VK_FORMAT_R16_SFLOAT);
        CHECK(ToVkType(TextureFormat::RGB565) == VK_FORMAT_R5G6B5_UNORM_PACK16);
        CHECK(ToVkType(TextureFormat::RGBA1010102) == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
    }
    TEST_CASE("R32F maps to VK_FORMAT_R32_SFLOAT") {
        // Single-channel 32-bit float — used by the depth-to-color resolve
        // RT (path D fallback in the volumetric chain).  R16F would lose
        // ~3 bits of precision near the far plane (NDC z ≈ 1.0 quantises to
        // ~1e-3 in R16F), which flickers the ray-march clamp on long-range
        // scenes (default FarClip is 1000m).
        CHECK(ToVkType(TextureFormat::R32F) == VK_FORMAT_R32_SFLOAT);
    }
}
