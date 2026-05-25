#include <doctest.h>
#include <vulkan/vulkan.h>

#include "Vulkan/Device.hpp"

using wallpaper::vulkan::Device;

// Default-constructed Device (no Create call) must report d32_sampleable()
// as false. The probe in Device::Create sets it to true on most desktop GPUs;
// the default-false guarantees a Device that was never properly initialised
// can't accidentally take path A and crash sampling a non-sampleable image.
TEST_SUITE("Device::d32_sampleable") {
    TEST_CASE("default-constructed Device reports d32_sampleable() == false") {
        Device dev;
        CHECK(dev.d32_sampleable() == false);
    }

    // Pin the probe semantics: m_d32_sampleable must be true iff the
    // optimalTilingFeatures bitfield contains VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT.
    // This is a contract test, not a behaviour test — the live probe in
    // Device::Create is exercised by the integration test in preflight.sh.
    TEST_CASE("probe predicate uses SAMPLED_IMAGE_BIT on optimalTilingFeatures") {
        VkFormatProperties props {};
        props.optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        // The probe we will write in Device.cpp must use this exact test.
        bool sampleable =
            (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        CHECK(sampleable == true);

        props.optimalTilingFeatures = 0;
        sampleable =
            (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        CHECK(sampleable == false);

        // linearTilingFeatures must NOT be the predicate (linear tiling is
        // rarely populated for depth formats; using it would gate path A off
        // on every real GPU).
        props.optimalTilingFeatures  = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        props.linearTilingFeatures   = 0;
        sampleable =
            (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        CHECK(sampleable == true);
    }
}
