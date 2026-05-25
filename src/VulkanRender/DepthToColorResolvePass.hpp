#pragma once

#include "VulkanPass.hpp"
#include "SpecTexs.hpp"

#include "Vulkan/Device.hpp"
#include "Vulkan/Parameters.hpp"
#include "Vulkan/GraphicsPipeline.hpp"

#include <string>
#include <vulkan/vulkan.h>

namespace wallpaper
{

namespace vulkan
{

// Path-D fallback for the volumetric depth-sample chain.  When the device
// rejects VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT on VK_FORMAT_D32_SFLOAT
// (Device::d32_sampleable() == false), this pass runs after the opaque chain
// and writes gl_FragCoord.z into a single-channel R32_SFLOAT color RT
// (_rt_sceneDepthLinear).  Downstream consumers alias _rt_sceneDepth to that
// RT so the rest of the chain is path-agnostic.
//
// Leg 01 ships only the header surface + a stub; leg 04 supplies the
// fullscreen shader and the render-graph wiring.
class DepthToColorResolvePass : public VulkanPass {
public:
    struct Desc {
        // in
        VkImage     input_depth_image { VK_NULL_HANDLE };
        VkImageView input_depth_view { VK_NULL_HANDLE };
        VkExtent2D  extent { 0, 0 };

        // out
        std::string output { std::string(WE_SCENE_DEPTH_LINEAR) };

        // pipeline (populated by prepare)
        VkFormat           output_format { VK_FORMAT_R32_SFLOAT };
        ImageParameters    vk_output;
        VkClearValue       clear_value;
        vvk::Framebuffer   fb;
        PipelineParameters pipeline;
    };

    DepthToColorResolvePass(const Desc& desc);
    ~DepthToColorResolvePass() override;

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

    const Desc& desc() const { return m_desc; }

private:
    Desc m_desc;
};

} // namespace vulkan
} // namespace wallpaper
