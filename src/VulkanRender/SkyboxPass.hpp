#pragma once
#include "VulkanPass.hpp"
#include "FramebufferCache.hpp"
#include <array>
#include <string>

#include "Vulkan/Device.hpp"
#include "Vulkan/StagingBuffer.hpp"
#include "Vulkan/GraphicsPipeline.hpp"

#include "Scene/Scene.h"
#include "SpecTexs.hpp"

namespace wallpaper
{
namespace vulkan
{

// Depth-disabled fullscreen background pass that samples an equirectangular
// panorama into _rt_default before the scene's own layers composite on top.
// Emitted only for scenes tagged has_skybox (see emitSkyboxPass); non-skybox
// scenes never build one, so the flat render path is byte-identical.
//
// The renderpass / framebuffer / descriptor / barrier boilerplate is cloned
// from FinPass — a proven color-attachment pass — with two changes: the output
// is the scene _rt_default render target (not the swapchain present image), and
// the fragment shader samples an equirect panorama by a per-fragment world
// direction instead of a straight texcoord.  For Increment A the camera is not
// yet wired: invViewProj is identity, giving a static background.  A later
// increment feeds the active camera's inverse view-projection so the sampled
// direction tracks the scene camera.
class SkyboxPass : public VulkanPass {
public:
    struct Desc {
        // in — declared by emitSkyboxPass.
        std::string output;       // render target written (SpecTex_Default)
        std::string pano_tex_key; // equirect panorama sampled (scene.skyboxTexKey)

        // prepared
        ImageParameters vk_output; // resolved _rt_default target
        ImageParameters vk_pano;   // resolved panorama texture
        VkClearValue    clear_value;

        StagingBufferRef   vertex_buf;
        StagingBufferRef   ubo_buf;
        PipelineParameters pipeline;
    };

    SkyboxPass(const Desc&);
    virtual ~SkyboxPass();

    const Desc& desc() const { return m_desc; }

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    Desc                                            m_desc;
    FramebufferCache<VkImageView, vvk::Framebuffer> m_fb_cache;
};

} // namespace vulkan
} // namespace wallpaper
