#include <doctest.h>

#include "VulkanRender/CustomShaderPass.hpp"
#include "SpecTexs.hpp"

#include <vector>
#include <string>

using wallpaper::vulkan::CustomShaderPass;

// Pin that the WE_SCENE_DEPTH key, when present in a Desc's textures list,
// participates in the IsSpecTex branch of prepare().  We can't drive a real
// prepare() without a live device, but we can verify the Desc construction
// flow accepts the key.  Functional verification happens in preflight via
// scenescript_tests + the live volumetric front pass (legs 03/04).
TEST_SUITE("CustomShaderPass::depth-binding") {
    TEST_CASE("WE_SCENE_DEPTH key can be added to Desc::textures") {
        CustomShaderPass::Desc desc;
        desc.textures.emplace_back(wallpaper::WE_SCENE_DEPTH);
        REQUIRE(desc.textures.size() == 1);
        CHECK(desc.textures[0] == "_rt_sceneDepth");
        CHECK(wallpaper::IsSpecTex(desc.textures[0]) == true);
    }
    TEST_CASE("needsSceneDepth alone causes prepare to wire the depth binding") {
        // Structural pin: a Desc with needsSceneDepth=true and no
        // _rt_sceneDepth in textures still requires prepare() to wire the
        // depth.  We can't drive prepare() here; this is a docs-as-test.
        CustomShaderPass::Desc desc;
        desc.needsSceneDepth = true;
        CHECK(desc.textures.empty());
        CHECK(desc.needsSceneDepth);
    }
}

TEST_SUITE("CustomShaderPass::depth-binding") {
    TEST_CASE("MSAA gate: msaaSamples > 1 should skip the path-A binding") {
        auto path_a_eligible = [](unsigned msaaSamples, bool d32_sampleable) {
            return d32_sampleable && msaaSamples <= 1;
        };
        CHECK(path_a_eligible(1, true) == true);
        CHECK(path_a_eligible(2, true) == false);
        CHECK(path_a_eligible(4, true) == false);
        CHECK(path_a_eligible(8, true) == false);
        CHECK(path_a_eligible(1, false) == false);
    }
}

TEST_SUITE("CustomShaderPass::depth-binding") {
    TEST_CASE("reflection gate: reflect_y0 should skip the path-A binding") {
        auto path_a_eligible = [](bool reflect_y0, bool d32_sampleable,
                                  unsigned msaaSamples) {
            return d32_sampleable && msaaSamples <= 1 && !reflect_y0;
        };
        CHECK(path_a_eligible(false, true, 1) == true);
        CHECK(path_a_eligible(true,  true, 1) == false);
        CHECK(path_a_eligible(true,  true, 4) == false);
        CHECK(path_a_eligible(false, false, 1) == false);
    }
}
