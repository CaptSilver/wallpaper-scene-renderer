#include "CopyPass.hpp"
#include "SpecTexs.hpp"
#include "Utils/Logging.h"
#include "Utils/AutoDeletor.hpp"
#include "Resource.hpp"
#include "PassCommon.hpp"

using namespace wallpaper::vulkan;

CopyPass::CopyPass(const Desc& desc): m_desc(desc) {}

CopyPass::~CopyPass() {};

void CopyPass::prepare(Scene& scene, const Device& device, RenderingResources& rr) {
    if (scene.renderTargets.count(m_desc.src) == 0) {
        LOG_ERROR("%s not found", m_desc.src.c_str());
        return;
    }
    if (scene.renderTargets.count(m_desc.dst) == 0) {
        auto& rt                                   = scene.renderTargets.at(m_desc.src);
        scene.renderTargets[m_desc.dst]            = rt;
        scene.renderTargets[m_desc.dst].allowReuse = true;
    }

    std::array<std::string, 2>      textures    = { m_desc.src, m_desc.dst };
    std::array<ImageParameters*, 2> vk_textures = { &m_desc.vk_src, &m_desc.vk_dst };
    for (usize i = 0; i < textures.size(); i++) {
        auto& tex_name = textures[i];
        if (tex_name.empty()) continue;

        ImageParameters img;
        if (IsSpecTex(tex_name)) {
            auto& rt  = scene.renderTargets.at(tex_name);
            auto  opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
            if (opt.has_value())
                img = opt.value();
            else
                LOG_ERROR("query image from cache failed");
        } else {
            LOG_ERROR("can't copy image source");
            return;
        }
        *vk_textures[i] = img;
    }

    for (auto& tex : releaseTexs()) {
        device.tex_cache().MarkShareReady(tex);
    }

    LOG_INFO("CopyPass: '%s' %ux%u -> '%s' %ux%u (%s%s)",
             m_desc.src.c_str(), m_desc.vk_src.extent.width, m_desc.vk_src.extent.height,
             m_desc.dst.c_str(), m_desc.vk_dst.extent.width, m_desc.vk_dst.extent.height,
             (! m_desc.flipY &&
              m_desc.vk_src.extent.width == m_desc.vk_dst.extent.width &&
              m_desc.vk_src.extent.height == m_desc.vk_dst.extent.height)
                 ? "copy" : "blit",
             m_desc.flipY ? ", flipY" : "");

    setPrepared();
};
void CopyPass::execute(const Device& device, RenderingResources& rr) {
    // First-frame execution trace for copy passes
    {
        extern int g_exec_pass_counter;
        extern int g_exec_frame_counter;
        if (g_exec_frame_counter < 1) {
            LOG_INFO("EXEC[%d] pass#%d COPY '%.*s' %ux%u -> '%.*s' %ux%u",
                     g_exec_frame_counter, g_exec_pass_counter++,
                     (int)m_desc.src.size(), m_desc.src.data(),
                     m_desc.vk_src.extent.width, m_desc.vk_src.extent.height,
                     (int)m_desc.dst.size(), m_desc.dst.data(),
                     m_desc.vk_dst.extent.width, m_desc.vk_dst.extent.height);
        }
    }
    auto& cmd = rr.command;
    auto& src = m_desc.vk_src;
    auto& dst = m_desc.vk_dst;

    if (! (src.handle && dst.handle)) {
        LOG_ERROR("CopyPass: invalid handle src=%p dst=%p", (void*)src.handle, (void*)dst.handle);
        assert(src.handle && dst.handle);
        return;
    }

    VkImageSubresourceRange srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,

    };
    // flipY requires the blit path (VkImageCopy can't flip)
    bool size_match = ! m_desc.flipY &&
                      src.extent.width == dst.extent.width &&
                      src.extent.height == dst.extent.height;

    VkImageSubresourceLayers subresource {
        .aspectMask     = srang.aspectMask,
        .mipLevel       = 0,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };

    {
        VkImageMemoryBarrier in_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image            = src.handle,
            .subresourceRange = srang,
        };
        VkImageMemoryBarrier out_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image            = dst.handle,
            .subresourceRange = srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            {},
                            {},
                            std::array { in_bar, out_bar });
    }

    if (size_match) {
        VkImageCopy copy {
            .srcSubresource = subresource,
            .dstSubresource = subresource,
            .extent         = { src.extent.width, src.extent.height, 1 },
        };
        cmd.CopyImage(src.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      dst.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      copy);
    } else {
        // Flip source Y when flipY is set (used for reflection render targets).
        // vkCmdBlitImage natively supports this via swapped srcOffsets.
        int32_t srcY0 = m_desc.flipY ? (int32_t)src.extent.height : 0;
        int32_t srcY1 = m_desc.flipY ? 0 : (int32_t)src.extent.height;
        VkImageBlit blit {
            .srcSubresource = subresource,
            .srcOffsets     = { VkOffset3D { 0, srcY0, 0 },
                                VkOffset3D { (int32_t)src.extent.width, srcY1, 1 } },
            .dstSubresource = subresource,
            .dstOffsets     = { VkOffset3D { 0, 0, 0 },
                                VkOffset3D { (int32_t)dst.extent.width,
                                             (int32_t)dst.extent.height, 1 } },
        };
        cmd.BlitImage(src.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      dst.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      blit,
                      VK_FILTER_LINEAR);
    }
    {
        VkImageMemoryBarrier in_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = src.handle,
            .subresourceRange = srang,
        };
        VkImageMemoryBarrier out_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = dst.handle,
            .subresourceRange = srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            {},
                            {},
                            std::array { in_bar, out_bar });
    }

    if (dst.mipmap_level > 1) {
        device.tex_cache().RecGenerateMipmaps(cmd, dst);
    }
};
void CopyPass::destory(const Device&, RenderingResources&) {}
