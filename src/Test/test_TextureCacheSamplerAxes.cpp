#include <doctest.h>

#include <vulkan/vulkan.h>

#include "Vulkan/TextureCache.hpp"

using wallpaper::TextureSample;
using wallpaper::TextureWrap;
using wallpaper::vulkan::GenSamplerInfo;

// Each texture axis must take its address mode from its own wrap flag: U from
// wrapS, V from wrapT.  Sphere and equirectangular maps are the common case
// that needs them to differ — repeat around the equator, clamp at the poles —
// and a V that silently mirrors U turns the top and bottom rows into a wrapped
// smear.
TEST_SUITE("TextureCache sampler axes") {
    TEST_CASE("V follows wrapT when the two wrap flags differ") {
        TextureSample sample {};
        sample.wrapS = TextureWrap::REPEAT;
        sample.wrapT = TextureWrap::CLAMP_TO_EDGE;

        const auto info = GenSamplerInfo(sample, 1.0f, 1.0f);

        CHECK(info.addressModeU == VK_SAMPLER_ADDRESS_MODE_REPEAT);
        CHECK(info.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }

    TEST_CASE("swapping the wrap flags swaps the axes") {
        TextureSample sample {};
        sample.wrapS = TextureWrap::CLAMP_TO_EDGE;
        sample.wrapT = TextureWrap::REPEAT;

        const auto info = GenSamplerInfo(sample, 1.0f, 1.0f);

        CHECK(info.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        CHECK(info.addressModeV == VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
}
