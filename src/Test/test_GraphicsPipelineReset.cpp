#include <doctest.h>

#include "Vulkan/GraphicsPipeline.hpp"

using wallpaper::vulkan::PipelineParameters;
using wallpaper::vulkan::ResetPipelineParameters;

// FinPass keeps one PipelineParameters for the life of the process and calls
// GraphicsPipeline::create() on it again every scene switch; SkyboxPass shares
// the same shape even though it currently gets a fresh instance per scene.
// Nothing here can build a live VkDescriptorSetLayout without a real Vulkan
// device, so these cases drive PipelineParameters the same way create()'s
// append loop and a pass's destory() do, using default-constructed vvk
// handles (a null handle never reaches the driver, so this stays headless).
TEST_SUITE("GraphicsPipeline::ResetPipelineParameters") {
    TEST_CASE("clears descriptor_layouts and releases layout/handle") {
        PipelineParameters pipeline;
        for (int i = 0; i < 40; i++) pipeline.descriptor_layouts.emplace_back();
        REQUIRE(pipeline.descriptor_layouts.size() == 40);

        ResetPipelineParameters(pipeline);

        CHECK(pipeline.descriptor_layouts.empty());
        CHECK_FALSE(pipeline.layout);
        CHECK_FALSE(pipeline.handle);
    }

    TEST_CASE("40 prepare/destory-shaped cycles never let the layout count climb") {
        PipelineParameters pipeline;
        for (int scene_switch = 0; scene_switch < 40; scene_switch++) {
            // create() resets before appending this call's one descriptor-set
            // layout (FinPass and SkyboxPass each pass exactly one entry).
            ResetPipelineParameters(pipeline);
            pipeline.descriptor_layouts.emplace_back();
            REQUIRE(pipeline.descriptor_layouts.size() == 1);

            // destory() resets again so the next prepare() starts clean.
            ResetPipelineParameters(pipeline);
        }
        CHECK(pipeline.descriptor_layouts.empty());
    }
}
