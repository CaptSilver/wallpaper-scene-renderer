#include <doctest.h>

#include <vulkan/vulkan.h>

#include "Vulkan/TextureCache.hpp"

using wallpaper::vulkan::GenDepthSamplerInfo;

// Pin the depth-sampler configuration.  These properties are load-bearing:
// - NEAREST filter avoids interpolating NDC z across silhouettes (would give
//   nonsense intermediate z values).
// - CLAMP_TO_EDGE prevents wraparound at frame edges from re-introducing far-
//   plane z near image borders.
// - compareEnable=FALSE because the volumetric shader does its own compare
//   against backDepth (hardware PCF semantics would conflate two concerns).
// - mipmapMode=NEAREST + maxLod=0 ties the sampler to level 0 since the
//   depth attachment is single-mip.
TEST_SUITE("DepthSamplerConfig") {
    TEST_CASE("magFilter == VK_FILTER_NEAREST") {
        auto info = GenDepthSamplerInfo();
        CHECK(info.magFilter == VK_FILTER_NEAREST);
    }
    TEST_CASE("minFilter == VK_FILTER_NEAREST") {
        auto info = GenDepthSamplerInfo();
        CHECK(info.minFilter == VK_FILTER_NEAREST);
    }
    TEST_CASE("mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST") {
        auto info = GenDepthSamplerInfo();
        CHECK(info.mipmapMode == VK_SAMPLER_MIPMAP_MODE_NEAREST);
    }
    TEST_CASE("maxLod == 0") {
        auto info = GenDepthSamplerInfo();
        CHECK(info.maxLod == doctest::Approx(0.0f));
    }
    TEST_CASE("addressModeU/V == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE") {
        auto info = GenDepthSamplerInfo();
        CHECK(info.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        CHECK(info.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }
    TEST_CASE("compareEnable == VK_FALSE") {
        auto info = GenDepthSamplerInfo();
        CHECK(info.compareEnable == VK_FALSE);
    }
    TEST_CASE("anisotropyEnable == VK_FALSE") {
        auto info = GenDepthSamplerInfo();
        CHECK(info.anisotropyEnable == VK_FALSE);
        CHECK(info.maxAnisotropy == doctest::Approx(1.0f));
    }
}

// The live GetOrCreateDepthSampler() needs a Vulkan device; we can only pin
// the method's declared signature here (compile-time contract).  Functional
// verification: preflight.sh's sceneviewer-script smoke (creates a real
// device + samples depth) tells us the live path works.
TEST_SUITE("DepthSamplerConfig") {
    TEST_CASE("TextureCache::GetOrCreateDepthSampler is declared") {
        // Take the address-of as a non-null pointer; this fails to compile
        // if the method signature changes or is removed.
        auto method_ptr = &wallpaper::vulkan::TextureCache::GetOrCreateDepthSampler;
        CHECK(method_ptr != nullptr);
    }
}
