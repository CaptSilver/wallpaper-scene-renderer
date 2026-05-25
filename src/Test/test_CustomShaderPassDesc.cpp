#include <doctest.h>

#include "VulkanRender/CustomShaderPass.hpp"

using wallpaper::vulkan::CustomShaderPass;

// Pin the default-constructed Desc's capability flags.  needsSceneDepth must
// default to false so existing passes continue to behave exactly as before;
// only volumetric-front (and future SSAO/DOF) flips it on.
TEST_SUITE("CustomShaderPass::Desc") {
    TEST_CASE("needsSceneDepth defaults to false") {
        CustomShaderPass::Desc desc;
        CHECK(desc.needsSceneDepth == false);
    }
    TEST_CASE("disableDepth / flipCullMode / useReflectionDepth still default false") {
        // Regression guard — adding needsSceneDepth in the same struct must
        // not flip any other flag's default.
        CustomShaderPass::Desc desc;
        CHECK(desc.disableDepth == false);
        CHECK(desc.flipCullMode == false);
        CHECK(desc.useReflectionDepth == false);
    }
}
