#pragma once
#include "VulkanPass.hpp"
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

class FinPass : public VulkanPass {
public:
    struct Desc {
        // in
        const std::string_view result { SpecTex_Default };
        VkFormat               present_format;
        VkImageLayout          present_layout;
        uint32_t               present_queue_index;
        bool                   hdr_passthrough { false };
        bool                   hdr_content { false };

        // prepared
        ImageParameters vk_result;
        ImageParameters vk_present;
        VkImageLayout   render_layout;
        VkClearValue    clear_value;

        StagingBufferRef   vertex_buf;
        vvk::Framebuffer   fb;
        PipelineParameters pipeline;
    };

    FinPass(const Desc&);
    virtual ~FinPass();

    void setPresent(ImageParameters);
    void setPresentLayout(VkImageLayout);
    void setPresentFormat(VkFormat);
    void setPresentQueueIndex(uint32_t);
    void setHdrPassthrough(bool hdr);
    void setHdrContent(bool hdr);
    bool hdrContent() const { return m_desc.hdr_content; }
    // Force re-prepare so the tonemap shader is rebuilt with the current
    // hdr_content/hdr_passthrough setting.  Called when scene HDR mode changes.
    void markNeedsReprepare() { setPrepared(false); }

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

private:
    Desc m_desc;
};

} // namespace vulkan
} // namespace wallpaper
