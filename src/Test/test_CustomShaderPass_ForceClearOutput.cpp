#include <doctest.h>

#include "VulkanRender/CustomShaderPass.hpp"

using namespace wallpaper::vulkan;

// CustomShaderPass's ctor/dtor live in wpVulkanRender, which the headless
// test target does not link (mirrors test_CustomShaderPassDesc.cpp's pattern).
// Desc itself carries vvk::Framebuffer (move-only RAII handle), so it is
// non-copyable — we can only pin the default value of the new boolean here.
// The field-copy in CustomShaderPass::CustomShaderPass(const Desc&) is
// exercised end-to-end by the live renderer + sceneviewer preflight legs.
TEST_SUITE("CustomShaderPass_ForceClearOutput") {
    TEST_CASE("default-constructed Desc has force_clear_output = false") {
        CustomShaderPass::Desc d;
        CHECK_FALSE(d.force_clear_output);
    }

    TEST_CASE("force_clear_output is independently mutable on the Desc") {
        CustomShaderPass::Desc d;
        d.force_clear_output = true;
        CHECK(d.force_clear_output == true);
        // Regression guard: flipping force_clear_output must not perturb
        // the other capability flags that landed alongside it.
        CHECK(d.disableDepth == false);
        CHECK(d.flipCullMode == false);
        CHECK(d.useReflectionDepth == false);
        CHECK(d.needsSceneDepth == false);
    }
}

TEST_SUITE("CustomShaderPass_SelectOutputLoadOp") {
    TEST_CASE("untouched RT clears regardless of force_clear") {
        CHECK(SelectOutputLoadOp(false, false) == VK_ATTACHMENT_LOAD_OP_CLEAR);
        CHECK(SelectOutputLoadOp(true,  false) == VK_ATTACHMENT_LOAD_OP_CLEAR);
    }
    TEST_CASE("already-cleared RT loads when force_clear is false") {
        CHECK(SelectOutputLoadOp(false, true) == VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    TEST_CASE("already-cleared RT clears when force_clear is true") {
        CHECK(SelectOutputLoadOp(true, true) == VK_ATTACHMENT_LOAD_OP_CLEAR);
    }
}
