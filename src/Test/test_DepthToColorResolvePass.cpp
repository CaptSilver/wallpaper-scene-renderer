#include <doctest.h>

#include "VulkanRender/DepthToColorResolvePass.hpp"
#include "SpecTexs.hpp"

using wallpaper::vulkan::DepthToColorResolvePass;

TEST_SUITE("DepthToColorResolvePass") {
    TEST_CASE("output key is WE_SCENE_DEPTH_LINEAR (_rt_sceneDepthLinear)") {
        DepthToColorResolvePass::Desc desc;
        CHECK(desc.output == wallpaper::WE_SCENE_DEPTH_LINEAR);
        CHECK(desc.output == "_rt_sceneDepthLinear");
    }
    TEST_CASE("default Desc has no input bound") {
        DepthToColorResolvePass::Desc desc;
        CHECK(desc.input_depth_image == VK_NULL_HANDLE);
        CHECK(desc.input_depth_view  == VK_NULL_HANDLE);
    }
}
