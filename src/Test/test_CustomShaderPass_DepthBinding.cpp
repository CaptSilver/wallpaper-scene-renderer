#include <doctest.h>

#include "VulkanRender/CustomShaderPass.hpp"
#include "SpecTexs.hpp"

#include <vector>
#include <string>

using wallpaper::vulkan::CustomShaderPass;
using wallpaper::vulkan::DepthAliasPathAEligible;
using wallpaper::vulkan::EnsureSceneDepthTextureIfNeeded;

// Binding the depth attachment needs a live device, so these cases take the
// part of the path that does not: the WE_SCENE_DEPTH key itself (its spelling,
// and that IsSpecTex routes it into prepare()'s depth branch at all), the
// append-once rule of EnsureSceneDepthTextureIfNeeded, and the predicate
// prepare() asks before it samples the live depth attachment.  Whether the
// descriptor then lands on the GPU is a question for a run on real hardware.
TEST_SUITE("CustomShaderPass::depth-binding") {
    TEST_CASE("WE_SCENE_DEPTH key can be added to Desc::textures") {
        CustomShaderPass::Desc desc;
        desc.textures.emplace_back(wallpaper::WE_SCENE_DEPTH);
        REQUIRE(desc.textures.size() == 1);
        CHECK(desc.textures[0] == "_rt_sceneDepth");
        CHECK(wallpaper::IsSpecTex(desc.textures[0]) == true);
    }
    TEST_CASE("needsSceneDepth appends WE_SCENE_DEPTH exactly once, and never when unset") {
        CustomShaderPass::Desc desc;
        desc.needsSceneDepth = true;
        CHECK(EnsureSceneDepthTextureIfNeeded(desc.textures, desc.needsSceneDepth));
        REQUIRE(desc.textures.size() == 1);
        CHECK(desc.textures[0] == wallpaper::WE_SCENE_DEPTH);
        // Calling it again with the key already present is a no-op — the
        // second call must not duplicate the entry.
        CHECK_FALSE(EnsureSceneDepthTextureIfNeeded(desc.textures, desc.needsSceneDepth));
        CHECK(desc.textures.size() == 1);

        CustomShaderPass::Desc off;
        CHECK_FALSE(EnsureSceneDepthTextureIfNeeded(off.textures, off.needsSceneDepth));
        CHECK(off.textures.empty());
    }
    TEST_CASE("MSAA gate: msaaSamples > 1 should skip the path-A binding") {
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/true,
                                      /*d32_sampleable=*/true,
                                      /*msaaSamples=*/1,
                                      /*useReflectionDepth=*/false) == true);
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/true,
                                      /*d32_sampleable=*/true,
                                      /*msaaSamples=*/2,
                                      /*useReflectionDepth=*/false) == false);
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/true,
                                      /*d32_sampleable=*/true,
                                      /*msaaSamples=*/4,
                                      /*useReflectionDepth=*/false) == false);
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/true,
                                      /*d32_sampleable=*/true,
                                      /*msaaSamples=*/8,
                                      /*useReflectionDepth=*/false) == false);
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/true,
                                      /*d32_sampleable=*/false,
                                      /*msaaSamples=*/1,
                                      /*useReflectionDepth=*/false) == false);
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/false,
                                      /*d32_sampleable=*/true,
                                      /*msaaSamples=*/1,
                                      /*useReflectionDepth=*/false) == false);
    }
    TEST_CASE("reflection gate: useReflectionDepth should skip the path-A binding") {
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/true,
                                      /*d32_sampleable=*/true,
                                      /*msaaSamples=*/1,
                                      /*useReflectionDepth=*/false) == true);
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/true,
                                      /*d32_sampleable=*/true,
                                      /*msaaSamples=*/1,
                                      /*useReflectionDepth=*/true) == false);
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/true,
                                      /*d32_sampleable=*/true,
                                      /*msaaSamples=*/4,
                                      /*useReflectionDepth=*/true) == false);
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/true,
                                      /*d32_sampleable=*/false,
                                      /*msaaSamples=*/1,
                                      /*useReflectionDepth=*/false) == false);
        CHECK(DepthAliasPathAEligible(/*is_depth_alias=*/false,
                                      /*d32_sampleable=*/true,
                                      /*msaaSamples=*/1,
                                      /*useReflectionDepth=*/false) == false);
    }
}
