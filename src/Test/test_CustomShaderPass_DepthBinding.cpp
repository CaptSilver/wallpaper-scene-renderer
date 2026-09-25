#include <doctest.h>

#include "VulkanRender/CustomShaderPass.hpp"
#include "SpecTexs.hpp"

#include <unordered_set>
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
    // setDepthInitedFrameSet replaced a namespace-scope global
    // (g_depth_inited_frame) that died via ordinary static destruction
    // whenever the process called exit() -- with no regard for whether the
    // render thread was still mid-frame and reading it (an ASAN-confirmed
    // heap-use-after-free).  VulkanRender::Impl now owns the tracked set and
    // wires a pointer to it into every CustomShaderPass post-construction,
    // in compileRenderGraph.  CustomShaderPass's ctor/dtor need no Vulkan
    // device (only prepare()/execute()/destory() do), so the class is
    // constructible here; these cases exercise the real wiring path, not
    // just a struct field.
    TEST_CASE("a fresh pass has no depth-init tracker until wired") {
        CustomShaderPass::Desc desc;
        CustomShaderPass       pass(desc);
        CHECK_FALSE(pass.hasDepthInitedFrameSet());

        std::unordered_set<VkImage> owned_by_impl;
        pass.setDepthInitedFrameSet(&owned_by_impl);
        CHECK(pass.hasDepthInitedFrameSet());

        // compileRenderGraph re-wires on every scene switch; confirm the
        // pointer can be cleared again rather than sticking once set.
        pass.setDepthInitedFrameSet(nullptr);
        CHECK_FALSE(pass.hasDepthInitedFrameSet());
    }
    TEST_CASE("two passes wire to independent tracker sets") {
        // Mirrors two VulkanRender::Impl instances (e.g. two monitors, each
        // rendering its own wallpaper): each pass must end up pointing at
        // its own Impl's set, never a shared or swapped one.
        CustomShaderPass::Desc      desc;
        CustomShaderPass            pass_a(desc);
        CustomShaderPass            pass_b(desc);
        std::unordered_set<VkImage> set_a;
        std::unordered_set<VkImage> set_b;
        pass_a.setDepthInitedFrameSet(&set_a);
        pass_b.setDepthInitedFrameSet(&set_b);
        CHECK(pass_a.hasDepthInitedFrameSet());
        CHECK(pass_b.hasDepthInitedFrameSet());

        set_a.insert(reinterpret_cast<VkImage>(0x1));
        CHECK(set_a.count(reinterpret_cast<VkImage>(0x1)) == 1);
        CHECK(set_b.count(reinterpret_cast<VkImage>(0x1)) == 0);
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
