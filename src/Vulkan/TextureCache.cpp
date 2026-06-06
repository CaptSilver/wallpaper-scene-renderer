
#include "Shader.hpp"
#include "Swapchain.hpp"
#include "TextureCache.hpp"
#include "Device.hpp"
#include "Util.hpp"

#include <algorithm>

#include "Image.hpp"
#include "Core/MapSet.hpp"
#include "Core/ArrayHelper.hpp"
#include "Utils/AutoDeletor.hpp"
#include "Utils/Hash.h"
#include "include/Vulkan/Parameters.hpp"
#include "vvk/vulkan_wrapper.hpp"
#include "TexFormatVk.hpp" // ToVkType(TextureFormat) — testable mapping (BC1 1-bit alpha)
#include "TextureCacheDetail.hpp" // MEM4: mipOffsets / packedTotalBytes / bytesPerBlockForFormat
#include "ImagePayload.h"  // mayReleaseDecodedPayload / releaseDecodedPayload

#include <cstdio>
#include <cstdlib>
#include <optional>

using namespace wallpaper;
using namespace wallpaper::vulkan;

namespace wallpaper
{
namespace vulkan
{
// ToVkType(TextureFormat) moved to include/Vulkan/TexFormatVk.hpp so it is
// unit-testable without the Vulkan device (see Test/test_TexFormatVk.cpp).

VkSamplerAddressMode ToVkType(wallpaper::TextureWrap sam) {
    using namespace wallpaper;
    switch (sam) {
    case TextureWrap::CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case TextureWrap::REPEAT:
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}
VkFilter ToVkType(wallpaper::TextureFilter sam) {
    using namespace wallpaper;
    switch (sam) {
    case TextureFilter::LINEAR: return VK_FILTER_LINEAR;
    case TextureFilter::NEAREST:
    default: return VK_FILTER_NEAREST;
    }
}

VkSamplerCreateInfo GenDepthSamplerInfo() {
    VkSamplerCreateInfo info { .sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .pNext            = nullptr,
                               .flags            = 0,
                               .magFilter        = VK_FILTER_NEAREST,
                               .minFilter        = VK_FILTER_NEAREST,
                               .mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .mipLodBias       = 0.0f,
                               .anisotropyEnable = VK_FALSE,
                               .maxAnisotropy    = 1.0f,
                               .compareEnable    = VK_FALSE,
                               .compareOp        = VK_COMPARE_OP_NEVER,
                               .minLod           = 0.0f,
                               .maxLod           = 0.0f,
                               .borderColor      = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
                               .unnormalizedCoordinates = VK_FALSE };
    return info;
}
} // namespace vulkan
} // namespace wallpaper

namespace
{
VkSamplerCreateInfo GenSamplerInfo(TextureKey key, float deviceMaxAnisotropy) {
    auto& sam = key.sample;

    bool useAniso = (sam.magFilter == TextureFilter::LINEAR) && deviceMaxAnisotropy > 1.0f;

    // maxLod gates the highest mip the hardware sampler may pick.  Set to
    // the key's actual mip count so trilinear can pick lower mips when the
    // image has them.  When mipmap_level == 1 (the normal case for current
    // pingpongs and _rt_default), maxLod is 1.0 — equivalent to the prior
    // hardcoded value.  When multi-mip RTs are ever re-enabled, this lifts
    // the artificial clamp without exposing the rotated-quad pixelation we
    // hit while experimenting with mip-gen for halo softening (see
    // naruto-shippuden-scenescript.md "Path-not-taken: pingpong mip chain").
    const float maxLod = std::max(1.0f, (float)key.mipmap_level);

    VkSamplerCreateInfo sampler_info { .sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                       .pNext            = nullptr,
                                       .magFilter        = ToVkType(sam.magFilter),
                                       .minFilter        = (ToVkType(sam.minFilter)),
                                       .mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                       .addressModeU     = (ToVkType(sam.wrapS)),
                                       .addressModeV     = (ToVkType(sam.wrapS)),
                                       .addressModeW     = (ToVkType(sam.wrapT)),
                                       .anisotropyEnable = useAniso,
                                       .maxAnisotropy    = useAniso ? deviceMaxAnisotropy : 1.0f,
                                       .compareEnable    = (false),
                                       .compareOp        = VK_COMPARE_OP_NEVER,
                                       .minLod           = (0.0f),
                                       .maxLod           = maxLod,
                                       .borderColor      = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
                                       .unnormalizedCoordinates = (false) };
    return sampler_info;
}

VkResult TransImgLayout(const vvk::Queue& queue, vvk::CommandBuffer& cmd,
                        const ImageParameters& image, VkImageLayout layout) {
    VkResult result;
    do {
        result = cmd.Begin(VkCommandBufferBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        });
        if (result != VK_SUCCESS) break;

        VkImageSubresourceRange subresourceRange {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount     = VK_REMAINING_ARRAY_LAYERS,
        };
        {
            VkImageMemoryBarrier out_bar {
                .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext            = nullptr,
                .srcAccessMask    = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
                .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout        = layout,
                .image            = image.handle,
                .subresourceRange = subresourceRange,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_DEPENDENCY_BY_REGION_BIT,
                                out_bar);
        }
        result = cmd.End();
        if (result != VK_SUCCESS) break;

        VkSubmitInfo sub_info {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext              = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers    = cmd.address(),
        };
        result = queue.Submit(sub_info);
    } while (false);
    return result;
}

std::optional<vvk::DeviceMemory> AllocateMemory(const vvk::Device& device, vvk::PhysicalDevice gpu,
                                                VkMemoryRequirements  reqs,
                                                VkMemoryPropertyFlags property,
                                                void*                 pNext = NULL) {
    VkPhysicalDeviceMemoryProperties pros = gpu.GetMemoryProperties().memoryProperties;
    for (uint32_t i = 0; i < pros.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1 << i)) && (pros.memoryTypes[i].propertyFlags & property)) {
            VkMemoryAllocateInfo memory_allocate_info { .sType =
                                                            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                                        .pNext           = pNext,
                                                        .allocationSize  = reqs.size,
                                                        .memoryTypeIndex = i };
            vvk::DeviceMemory    mem;
            VkResult             res = device.AllocateMemory(memory_allocate_info, mem);
            if (res == VK_SUCCESS) {
                return mem;
            } else {
                VVK_CHECK(res);
                return std::nullopt;
            }
        }
    }
    LOG_ERROR("vulkan allocate memory failed, no memory match requires");
    return std::nullopt;
}

std::optional<ExImageParameters> CreateExImage(uint32_t width, uint32_t height, VkFormat format,
                                               VkImageTiling tiling, VkSampler sampler,
                                               VkImageUsageFlags usage, const vvk::Device& device,
                                               const vvk::PhysicalDevice& gpu) {
    // Sampler resolved by the caller via TextureCache::GetOrCreateSampler — see MEM5.
    ExImageParameters image;
    image.sampler = sampler;
    do {
        VkExternalMemoryImageCreateInfo ex_info {
            .sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext       = NULL,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
        };
        VkExportMemoryAllocateInfo ex_mem_info { .sType =
                                                     VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
                                                 .pNext = NULL,
                                                 .handleTypes =
                                                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
        VkImageCreateInfo          info {
                     .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                     .pNext       = &ex_info,
                     .imageType   = VK_IMAGE_TYPE_2D,
                     .format      = format,
                     .extent      = VkExtent3D { .width = width, .height = height, .depth = 1 },
                     .mipLevels   = 1,
                     .arrayLayers = 1,
                     .samples     = VK_SAMPLE_COUNT_1_BIT,
                     .tiling      = tiling,
                     .usage       = usage,
                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                     .queueFamilyIndexCount = 0,
                     .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        image.extent = info.extent;

        VVK_CHECK_ACT(break, device.CreateImage(info, image.handle));

        image.mem_reqs = device.GetImageMemoryRequirements(*image.handle);

        if (auto opt = AllocateMemory(
                device, gpu, image.mem_reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ex_mem_info);
            opt.has_value()) {
            image.mem = std::move(opt.value());
        } else
            break;

        VVK_CHECK_ACT(break, image.handle.BindMemory(*image.mem, 0));
        {
            VkImageViewCreateInfo createinfo {
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext    = nullptr,
                .image    = *image.handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = format,
                .subresourceRange =
                    VkImageSubresourceRange {
                        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel   = 0,
                        .levelCount     = 1,
                        .baseArrayLayer = 0,
                        .layerCount     = 1,
                    },
            };
            VVK_CHECK_ACT(break, device.CreateImageView(createinfo, image.view));
        }
        // No CreateSampler here — see CreateImage above (MEM5 dedup).
        VVK_CHECK_ACT(break, image.mem.GetMemoryFdKHR(&image.fd));

        return image;

    } while (false);
    return std::nullopt;
}

inline std::optional<VmaImageParameters>
CreateImage(const Device& device, VkExtent3D extent, u32 miplevel, VkFormat format,
            VkSampler sampler, VkImageUsageFlags usage,
            VmaMemoryUsage mem_usage = VMA_MEMORY_USAGE_GPU_ONLY) {
    // Sampler is resolved by the caller (TextureCache::GetOrCreateSampler) so
    // identical sampler configs share one underlying VkSampler; the image just
    // holds the raw handle non-owning.
    VmaImageParameters image;
    image.sampler = sampler;
    do {
        VkImageCreateInfo info {
            .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                 = nullptr,
            .imageType             = VK_IMAGE_TYPE_2D,
            .format                = format,
            .extent                = extent,
            .mipLevels             = miplevel,
            .arrayLayers           = 1,
            .samples               = VK_SAMPLE_COUNT_1_BIT,
            .tiling                = VK_IMAGE_TILING_OPTIMAL,
            .usage                 = usage,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        image.extent = info.extent;
        VmaAllocationCreateInfo vma_info {};
        vma_info.usage = mem_usage;
        VVK_CHECK_ACT(break,
                      vvk::CreateImage(device.vma_allocator(), info, vma_info, image.handle));

        image.mipmap_level = miplevel;
        {
            // Multi-mip view for sampler binding — covers all levels so the
            // hardware sampler can pick lower mips via LOD selection.
            VkImageViewCreateInfo createinfo {
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext    = nullptr,
                .image    = *image.handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = format,
                .subresourceRange =
                    VkImageSubresourceRange {
                        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel   = 0,
                        .levelCount     = miplevel,
                        .baseArrayLayer = 0,
                        .layerCount     = 1,
                    },
            };
            VVK_CHECK_ACT(break, device.handle().CreateImageView(createinfo, image.view));
        }
        if (miplevel > 1) {
            // Separate single-mip view for render attachments — Vulkan requires
            // a color attachment view to cover exactly one mip level.  When
            // miplevel == 1, the ImageParameters wrapper aliases mip0_view to
            // view so callers don't need a branch.
            VkImageViewCreateInfo createinfo {
                .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext    = nullptr,
                .image    = *image.handle,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format   = format,
                .subresourceRange =
                    VkImageSubresourceRange {
                        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel   = 0,
                        .levelCount     = 1,
                        .baseArrayLayer = 0,
                        .layerCount     = 1,
                    },
            };
            VVK_CHECK_ACT(break,
                          device.handle().CreateImageView(createinfo, image.mip0_view));
        }
        // No CreateSampler here — `sampler` was resolved by the caller via
        // TextureCache::GetOrCreateSampler so identical configs reuse one
        // underlying VkSampler (owned by m_sampler_cache).  See MEM5.
        return image;
    } while (false);
    /*
    if (result != vk::Result::eSuccess) {
        device.DestroyImageParameters(image);
    }
    */
    return std::nullopt;
}

// MEM4: copy mip levels from a single packed staging buffer to the
// destination image.  Mirrors CopyImageData's shape (begin/barrier-in/N
// CopyBufferToImage/barrier-out/end/submit) but takes one staging buffer
// + per-mip bufferOffset instead of N independent buffers.  The
// VkBufferImageCopy::bufferOffset values must be multiples of the
// format's texel block size — see bytesPerBlockForFormat() in
// Vulkan/TextureCacheDetail.hpp.
inline VkResult CopyImageDataWithOffsets(const BufferParameters&        staging,
                                         std::span<const std::size_t>   mip_offsets,
                                         std::span<const VkExtent3D>    in_exts,
                                         const vvk::Queue&              queue,
                                         vvk::CommandBuffer&            cmd,
                                         const ImageParameters&         image) {
    VkResult result;
    do {
        result = cmd.Begin(VkCommandBufferBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        });
        if (result != VK_SUCCESS) break;

        VkImageSubresourceRange subresourceRange {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = (uint32_t)mip_offsets.size(),
            .baseArrayLayer = 0,
            .layerCount     = 1,
        };
        {
            VkImageMemoryBarrier in_bar {
                .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext            = nullptr,
                .srcAccessMask    = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .image            = image.handle,
                .subresourceRange = subresourceRange,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_DEPENDENCY_BY_REGION_BIT,
                                in_bar);
        }
        VkBufferImageCopy copy {
            .imageSubresource =
                VkImageSubresourceLayers {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
        };
        for (usize i = 0; i < mip_offsets.size(); i++) {
            copy.bufferOffset              = (VkDeviceSize)mip_offsets[i];
            copy.imageSubresource.mipLevel = (u32)i;
            copy.imageExtent               = in_exts[i];
            cmd.CopyBufferToImage(
                staging.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy);
        }
        {
            VkImageMemoryBarrier out_bar {
                .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext            = nullptr,
                .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
                .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image            = image.handle,
                .subresourceRange = subresourceRange,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_DEPENDENCY_BY_REGION_BIT,
                                out_bar);
        }
        result = cmd.End();
        if (result != VK_SUCCESS) break;

        VkSubmitInfo sub_info {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext              = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers    = cmd.address(),
        };
        result = queue.Submit(sub_info);
    } while (false);
    return result;
}

inline VkResult CopyImageData(std::span<const BufferParameters> in_bufs,
                              std::span<const VkExtent3D> in_exts, const vvk::Queue& queue,
                              vvk::CommandBuffer& cmd, const ImageParameters& image) {
    VkResult result;
    do {
        result = cmd.Begin(VkCommandBufferBeginInfo {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        });
        if (result != VK_SUCCESS) break;

        VkImageSubresourceRange subresourceRange {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = (uint32_t)in_bufs.size(),
            .baseArrayLayer = 0,
            .layerCount     = 1,
        };
        {
            VkImageMemoryBarrier in_bar {
                .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext            = nullptr,
                .srcAccessMask    = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .image            = image.handle,
                .subresourceRange = subresourceRange,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_DEPENDENCY_BY_REGION_BIT,
                                in_bar);
        }
        VkBufferImageCopy copy {
            .imageSubresource =
                VkImageSubresourceLayers {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
        };
        for (usize i = 0; i < in_bufs.size(); i++) {
            copy.imageSubresource.mipLevel = (u32)i;
            copy.imageExtent               = in_exts[i];
            cmd.CopyBufferToImage(
                in_bufs[i].handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy);
        }
        {
            VkImageMemoryBarrier out_bar {
                .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext            = nullptr,
                .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
                .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image            = image.handle,
                .subresourceRange = subresourceRange,
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_DEPENDENCY_BY_REGION_BIT,
                                out_bar);
        }
        result = cmd.End();
        if (result != VK_SUCCESS) break;

        VkSubmitInfo sub_info {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext              = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers    = cmd.address(),
        };
        result = queue.Submit(sub_info);
    } while (false);
    return result;
}
} // namespace

std::size_t TextureKey::HashValue(const TextureKey& k) {
    std::size_t seed { 0 };
    utils::hash_combine(seed, k.width);
    utils::hash_combine(seed, k.height);
    utils::hash_combine(seed, (int)k.usage);
    utils::hash_combine(seed, (int)k.format);
    utils::hash_combine(seed, (int)k.mipmap_level);

    utils::hash_combine(seed, (int)k.sample.wrapS);
    utils::hash_combine(seed, (int)k.sample.wrapT);
    utils::hash_combine(seed, (int)k.sample.magFilter);
    return seed;
}

std::optional<ExImageParameters> TextureCache::CreateExTex(uint32_t width, uint32_t height,
                                                           VkFormat format, VkImageTiling tiling) {
    VkSamplerCreateInfo sampler_info {
        .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext                   = nullptr,
        .magFilter               = VK_FILTER_NEAREST,
        .minFilter               = VK_FILTER_NEAREST,
        .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU            = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        .addressModeV            = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        .addressModeW            = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        .anisotropyEnable        = false,
        .maxAnisotropy           = 1.0f,
        .compareEnable           = false,
        .compareOp               = VK_COMPARE_OP_NEVER,
        .minLod                  = 0.0f,
        .maxLod                  = 1.0f,
        .borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = false,
    };

    VkSampler shared_sampler = GetOrCreateSampler(sampler_info);
    auto      opt            = CreateExImage(width,
                             height,
                             format,
                             tiling,
                             shared_sampler,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             m_device.device(),
                             m_device.gpu());
    if (opt.has_value()) {
        const auto& eximg = opt.value();

        if (! m_tex_cmd) allocateCmd();
        TransImgLayout(m_device.graphics_queue().handle, m_tex_cmd, eximg, VK_IMAGE_LAYOUT_GENERAL);
        VVK_CHECK(m_device.handle().WaitIdle());
    }
    return opt;
}

ImageSlotsRef TextureCache::CreateTex(Image& image) {
    if (exists(m_tex_map, image.key)) {
        return m_tex_map.at(image.key);
    }

    ImageSlots img_slots;

    if (! m_tex_cmd) allocateCmd();

    img_slots.slots.resize(image.slots.size());

    auto& sam      = image.header.sample;
    float maxAniso = m_device.maxAnisotropy();

    for (usize i = 0; i < image.slots.size(); i++) {
        auto& image_paras   = img_slots.slots[i];
        auto& image_slot    = image.slots[i];
        auto  mipmap_levels = image_slot.mipmaps.size();

        // check data
        if (! image_slot) return {};

        bool                useAniso = (sam.magFilter == TextureFilter::LINEAR) && maxAniso > 1.0f;
        VkSamplerCreateInfo sampler_info {
            .sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext                   = nullptr,
            .magFilter               = ToVkType(sam.magFilter),
            .minFilter               = (ToVkType(sam.minFilter)),
            .mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU            = (ToVkType(sam.wrapS)),
            .addressModeV            = (ToVkType(sam.wrapS)),
            .addressModeW            = (ToVkType(sam.wrapT)),
            .anisotropyEnable        = useAniso,
            .maxAnisotropy           = useAniso ? maxAniso : 1.0f,
            .compareEnable           = (false),
            .compareOp               = VK_COMPARE_OP_NEVER,
            .minLod                  = (0.0f),
            .maxLod                  = (float)mipmap_levels,
            .borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
            .unnormalizedCoordinates = (false),
        };
        VkFormat   format = ToVkType(image.header.format);
        VkExtent3D ext { (u32)image_slot.width, (u32)image_slot.height, 1 };

        VkSampler shared_sampler = GetOrCreateSampler(sampler_info);
        if (auto opt = CreateImage(m_device,
                                   ext,
                                   (u32)mipmap_levels,
                                   format,
                                   shared_sampler,
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            opt.has_value()) {
            image_paras = std::move(opt.value());
        } else
            break;

        // MEM4: pack all mip levels into one CPU-only VMA staging buffer
        // sized to the sum of mip byte counts; record one
        // vkCmdCopyBufferToImage per mip pointing at this single buffer
        // with the appropriate VkBufferImageCopy::bufferOffset.  Drops
        // per-texture allocation count from N mips → 1, eliminating the
        // VmaAllocation metadata overhead and page-rounding waste of the
        // legacy per-mip path.  Bit-exact: same bytes from same source to
        // same image — vkCmdCopyBufferToImage validates each
        // VkBufferImageCopy independently.
        //
        // Alignment: bufferOffset must be a multiple of the format's
        // texel block size (1 for uncompressed; 8 for BC1/BC4; 16 for
        // BC2/BC3/BC5/BC6H/BC7).  detail::bytesPerBlockForFormat returns
        // the right value for every format the engine loads, and
        // detail::mipOffsets rounds each per-mip starting offset up.
        std::vector<std::size_t> mip_sizes;
        mip_sizes.reserve(image_slot.mipmaps.size());
        for (const auto& mip : image_slot.mipmaps) {
            mip_sizes.push_back(mip.size);
        }

        const std::size_t blockSize = detail::bytesPerBlockForFormat(format);
        const auto        offsets   = detail::mipOffsets(mip_sizes, blockSize);
        const std::size_t total     = detail::packedTotalBytes(mip_sizes, offsets);

        VmaBufferParameters staging;
        (void)CreateStagingBuffer(m_device.vma_allocator(), total, staging);
        {
            void* v_data = nullptr;
            VVK_CHECK(staging.handle.MapMemory(&v_data));
            auto* dst = static_cast<std::uint8_t*>(v_data);
            for (usize j = 0; j < image_slot.mipmaps.size(); j++) {
                const auto& image_data = image_slot.mipmaps[j];
                memcpy(dst + offsets[j], image_data.data.get(), (u32)image_data.size);
            }
            staging.handle.UnMapMemory();
        }

        std::vector<VkExtent3D> extents;
        extents.reserve(image_slot.mipmaps.size());
        for (const auto& mip : image_slot.mipmaps) {
            extents.push_back(VkExtent3D { (u32)mip.width, (u32)mip.height, 1 });
        }

        CopyImageDataWithOffsets(staging,
                                 offsets,
                                 extents,
                                 m_device.graphics_queue().handle,
                                 m_tex_cmd,
                                 image_paras);

        m_device.handle().WaitIdle();
    }
    m_tex_map[image.key] = std::move(img_slots);
    // The GPU image is now authoritative; the decoded CPU mip bytes were the
    // staging source and are dead after the WaitIdle above.  Free them but
    // keep the Image shell so the parser cache (m_registered) still serves
    // ParseHeader and a duplicate CreateTex early-returns on the m_tex_map
    // key.  Video-texture placeholders are left intact (policy == false);
    // their per-frame frames come from the live decoder, not from this image.
    // WEKDE_KEEP_TEXBYTES is a zero-rebuild A/B / rollback escape hatch.
    if (mayReleaseDecodedPayload(image) && ! std::getenv("WEKDE_KEEP_TEXBYTES")) {
        if (std::getenv("WEKDE_DEBUG_TEXBYTES")) {
            isize freed = 0;
            for (auto& s : image.slots)
                for (auto& m : s.mipmaps) freed += m.size;
            LOG_INFO("TEXBYTES free '%s': %lld decoded CPU bytes released",
                     image.key.c_str(),
                     (long long)freed);
        }
        releaseDecodedPayload(image);
    }
    return m_tex_map[image.key];
}

void TextureCache::allocateCmd() {
    const auto& pool = m_device.cmd_pool();
    VVK_CHECK(pool.Allocate(1, VK_COMMAND_BUFFER_LEVEL_PRIMARY, m_tex_cmds));
    m_tex_cmd = vvk::CommandBuffer(m_tex_cmds[0], m_device.handle().Dispatch());
}

std::optional<VmaImageParameters> TextureCache::CreateTex(TextureKey tex_key) {
    VmaImageParameters image_paras;
    do {
        VkSamplerCreateInfo sam_info       = GenSamplerInfo(tex_key, m_device.maxAnisotropy());
        VkSampler           shared_sampler = GetOrCreateSampler(sam_info);
        VkFormat            format         = ToVkType(tex_key.format);
        VkExtent3D          ext { (u32)tex_key.width, (u32)tex_key.height, 1 };

        if (auto opt =
                CreateImage(m_device,
                            ext,
                            tex_key.mipmap_level,
                            format,
                            shared_sampler,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            opt.has_value()) {
            image_paras = std::move(opt.value());
        } else
            break;

        if (! m_tex_cmd) allocateCmd();
        TransImgLayout(m_device.graphics_queue().handle,
                       m_tex_cmd,
                       image_paras,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        VVK_CHECK_ACT(break, m_device.handle().WaitIdle());
        return image_paras;
    } while (false);
    return std::nullopt;
}

void TextureCache::evictColdQueryTexs() {
    // Build parallel lru_tick + persist views over m_query_texs so the
    // selection helper stays pure (testable without a live VkDevice).
    std::vector<uint64_t> ticks;
    std::vector<uint8_t>  persist_flags;
    std::vector<uint64_t> last_gens;
    ticks.reserve(m_query_texs.size());
    persist_flags.reserve(m_query_texs.size());
    last_gens.reserve(m_query_texs.size());
    for (const auto& q : m_query_texs) {
        ticks.push_back(q->lru_tick);
        persist_flags.push_back(q->persist ? 1u : 0u);
        last_gens.push_back(q->last_gen);
    }
    // Current-generation entries are live render targets in this scene (still
    // bound to a pass framebuffer); evicting one destroys an image still
    // referenced by CustomShaderPass::execute -> use-after-free.  Protect them
    // regardless of the soft cap; only prior-scene entries are reclaimable.
    const std::vector<std::uint8_t> persists =
        detail::effectiveEvictionPersist(persist_flags, last_gens, m_query_generation);
    const std::vector<std::size_t> victims =
        detail::selectEvictionVictims(ticks, persists, m_query_soft_cap);
    if (victims.empty()) return;
    // Defensive: drain in-flight frames so no submitted command buffer is
    // still referencing the evicted images.  Mirrors the WaitIdle invariant
    // around Clear() (which destroys the entire pool).  Called once per
    // eviction batch — eviction is rare (only above cap), so the cost
    // amortises.
    m_device.handle().WaitIdle();
    // Victims are sorted descending — erasing in this order keeps remaining
    // indices stable.
    for (std::size_t v : victims) {
        // Prune m_query_map of any keys that still point at the evicted
        // entry BEFORE the unique_ptr is destroyed (the map stores raw
        // QueryTex*, which would dangle).
        for (const auto& mapped_key : m_query_texs[v]->query_keys) {
            m_query_map.erase(mapped_key);
        }
        m_query_texs.erase(m_query_texs.begin() + v);
    }
    LOG_INFO("TextureCache: evicted %zu cold query-tex entries (cap=%u, size=%zu)",
             victims.size(),
             m_query_soft_cap,
             m_query_texs.size());
}

TextureCache::TextureCache(const Device& device): m_device(device) {
    const char* env = std::getenv("WEK_TEXCACHE_QUERY_CAP");
    if (env != nullptr && env[0] != '\0') {
        // Sentinel-based accept/reject probe: pass two distinct defaults and
        // check whether the parser kept either of them.  If both return the
        // same value, the env was accepted (and that value is the parse
        // result); otherwise the parser bailed to its default arg.  Avoids a
        // false "rejected" log when the user explicitly passes the current
        // default (e.g. WEK_TEXCACHE_QUERY_CAP=64).
        const uint32_t p1 = detail::parseQueryCapEnv(env, /*default=*/0, /*min=*/8, /*max=*/4096);
        const uint32_t p2 = detail::parseQueryCapEnv(env, /*default=*/1, /*min=*/8, /*max=*/4096);
        if (p1 == p2) {
            m_query_soft_cap = p1;
            LOG_INFO("TextureCache: query-tex soft cap = %u (WEK_TEXCACHE_QUERY_CAP override)",
                     m_query_soft_cap);
        } else {
            LOG_INFO("TextureCache: WEK_TEXCACHE_QUERY_CAP='%s' rejected "
                     "(outside [8, 4096] or malformed); using default cap = %u",
                     env,
                     m_query_soft_cap);
        }
    }
}

TextureCache::~TextureCache() {};

void TextureCache::Clear() {
    m_tex_map.clear();
    m_reupload_staging.clear();
    m_query_texs.clear();
    m_query_map.clear();
    // Clear the sampler cache LAST.  Samplers must be destroyed AFTER the
    // images that reference them (or after vkDeviceWaitIdle); the m_tex_map
    // clear above destroys the images synchronously, so this is the safe
    // point to drop the dedup'd VkSampler handles.  If a future refactor
    // reorders, --valid-layer flags "destroy in use" on the still-bound
    // descriptors.
    m_sampler_cache.clear();
}

std::optional<ImageParameters> TextureCache::Query(std::string_view key, TextureKey content_hash,
                                                   bool persist) {
    if (exists(m_query_map, key)) {
        auto& query = *(m_query_map.find(key)->second);

        query.share_ready = false;
        query.persist     = persist;
        query.lru_tick    = ++m_lru_clock;
        query.last_gen    = m_query_generation;

        return query.image;
    };

    TexHash tex_hash = TextureKey::HashValue(content_hash);
    for (auto& query : m_query_texs) {
        if (! (query->share_ready)) continue;
        if (query->content_hash != tex_hash) continue;

        query->share_ready = false;
        query->persist     = persist;
        query->query_keys.insert(std::string(key));
        query->lru_tick = ++m_lru_clock;
        query->last_gen = m_query_generation;

        m_query_map[std::string(key)] = &(*query);

        return query->image;
    }

    m_query_texs.emplace_back(std::make_unique<QueryTex>());
    auto& query                   = *m_query_texs.back();
    m_query_map[std::string(key)] = &query;

    query.index        = (idx)m_query_texs.size() - 1;
    query.content_hash = tex_hash;
    query.query_keys.insert(std::string(key));
    query.persist  = persist;
    query.lru_tick = ++m_lru_clock;
    query.last_gen = m_query_generation;
    if (auto opt = CreateTex(content_hash); opt.has_value()) {
        query.image = std::move(opt.value());
        if (m_query_texs.size() > m_query_soft_cap) {
            evictColdQueryTexs();
        }
        return query.image;
    }
    return std::nullopt;
}

void TextureCache::MarkShareReady(std::string_view key) {
    if (exists(m_query_map, key)) {
        auto& query = m_query_map.find(key)->second;
        if (query->persist) return;
        query->share_ready = true;
        m_query_map.erase(key.data());
    }
}

bool TextureCache::ReuploadTex(const std::string& key, Image& image) {
    if (! exists(m_tex_map, key)) {
        LOG_ERROR("ReuploadTex: key '%s' not found in cache", key.c_str());
        return false;
    }
    auto& img_slots = m_tex_map.at(key);
    if (img_slots.slots.empty() || image.slots.empty()) return false;

    if (! m_tex_cmd) allocateCmd();

    // Reuse persistent per-texture staging across frames (video re-upload),
    // recreating a slot's buffer only when its payload grows.  Avoids a
    // ~frame-sized alloc/free every frame; the WaitIdle below keeps reuse safe.
    auto& key_pool = m_reupload_staging[key];
    if (key_pool.size() < img_slots.slots.size()) key_pool.resize(img_slots.slots.size());

    for (usize i = 0; i < std::min(img_slots.slots.size(), image.slots.size()); i++) {
        auto& image_paras = img_slots.slots[i];
        auto& image_slot  = image.slots[i];
        if (image_slot.mipmaps.empty()) continue;

        const usize             mip_count  = image_slot.mipmaps.size();
        auto&                   stage_bufs = key_pool[i];
        std::vector<VkExtent3D> extents;
        if (stage_bufs.size() < mip_count) stage_bufs.resize(mip_count);

        for (usize j = 0; j < mip_count; j++) {
            auto& image_data = image_slot.mipmaps[j];
            auto& buf        = stage_bufs[j];
            if (! stagingBufferReusable(
                    static_cast<bool>(buf.handle), buf.req_size, image_data.size)) {
                VmaBufferParameters newbuf;
                (void)CreateStagingBuffer(m_device.vma_allocator(), (u32)image_data.size, newbuf);
                buf = std::move(newbuf);
            }
            {
                void* v_data;
                VVK_CHECK(buf.handle.MapMemory(&v_data));
                memcpy(v_data, image_data.data.get(), (u32)image_data.size);
                buf.handle.UnMapMemory();
            }
            extents.push_back(VkExtent3D { (u32)image_data.width, (u32)image_data.height, 1 });
        }

        // Re-upload: transition from SHADER_READ_ONLY → TRANSFER_DST, copy, → SHADER_READ_ONLY
        VkResult result;
        do {
            result = m_tex_cmd.Begin(VkCommandBufferBeginInfo {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            });
            if (result != VK_SUCCESS) break;

            VkImageSubresourceRange subresourceRange {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = (uint32_t)mip_count,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            };
            {
                VkImageMemoryBarrier in_bar {
                    .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext            = nullptr,
                    .srcAccessMask    = VK_ACCESS_SHADER_READ_BIT,
                    .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .image            = *image_paras.handle,
                    .subresourceRange = subresourceRange,
                };
                m_tex_cmd.PipelineBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_DEPENDENCY_BY_REGION_BIT,
                                          in_bar);
            }
            VkBufferImageCopy copy {
                .imageSubresource =
                    VkImageSubresourceLayers {
                        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseArrayLayer = 0,
                        .layerCount     = 1,
                    },
            };
            for (usize j = 0; j < mip_count; j++) {
                copy.imageSubresource.mipLevel = (u32)j;
                copy.imageExtent               = extents[j];
                m_tex_cmd.CopyBufferToImage(*stage_bufs[j].handle,
                                            *image_paras.handle,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            copy);
            }
            {
                VkImageMemoryBarrier out_bar {
                    .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext            = nullptr,
                    .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
                    .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .image            = *image_paras.handle,
                    .subresourceRange = subresourceRange,
                };
                m_tex_cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                          VK_DEPENDENCY_BY_REGION_BIT,
                                          out_bar);
            }
            result = m_tex_cmd.End();
            if (result != VK_SUCCESS) break;

            VkSubmitInfo sub_info {
                .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext              = nullptr,
                .commandBufferCount = 1,
                .pCommandBuffers    = m_tex_cmd.address(),
            };
            result = m_device.graphics_queue().handle.Submit(sub_info);
        } while (false);

        m_device.handle().WaitIdle();
        if (result != VK_SUCCESS) return false;
    }
    return true;
}

void TextureCache::RecGenerateMipmaps(vvk::CommandBuffer& cmd, const ImageParameters& image) const {
    VkImageMemoryBarrier barrier {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext               = nullptr,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image.handle,
        .subresourceRange =
            VkImageSubresourceRange {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
    };
    /*
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        out_bar);
        */

    i32 mipWidth  = (i32)image.extent.width;
    i32 mipHeight = (i32)image.extent.height;

    for (uint i = 1; i < image.mipmap_level; i++) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout                     = i == 1 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            barrier);

        barrier.subresourceRange.baseMipLevel = i;
        barrier.oldLayout                     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            barrier);

        VkImageBlit blit {
            .srcSubresource =
                VkImageSubresourceLayers {
                    .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel       = i - 1,
                    .baseArrayLayer = 0,
                    .layerCount     = 1,
                },
            .srcOffsets = { VkOffset3D { 0, 0, 0 }, VkOffset3D { mipWidth, mipHeight, 1 } },
            .dstOffsets = { VkOffset3D { 0, 0, 0 },
                            VkOffset3D { mipWidth > 1 ? mipWidth / 2 : 1,
                                         mipHeight > 1 ? mipHeight / 2 : 1,
                                         1 } },
        };
        blit.dstSubresource =
            VkImageSubresourceLayers {
                .aspectMask     = blit.srcSubresource.aspectMask,
                .mipLevel       = blit.srcSubresource.mipLevel + 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },

        cmd.BlitImage(image.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      image.handle,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      blit,
                      VK_FILTER_LINEAR);

        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout                     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            barrier);

        if (mipWidth > 1) mipWidth /= 2;
        if (mipHeight > 1) mipHeight /= 2;
    }

    barrier.subresourceRange.baseMipLevel = image.mipmap_level - 1;
    barrier.oldLayout                     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout                     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask                 = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask                 = VK_ACCESS_SHADER_READ_BIT;

    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_DEPENDENCY_BY_REGION_BIT,
                        barrier);
}

VkSampler TextureCache::GetOrCreateDepthSampler() {
    if (*m_depth_sampler != VK_NULL_HANDLE) {
        return *m_depth_sampler;
    }
    auto info = GenDepthSamplerInfo();
    if (m_device.handle().CreateSampler(info, m_depth_sampler) != VK_SUCCESS) {
        LOG_ERROR("depth sampler creation failed");
        return VK_NULL_HANDLE;
    }
    return *m_depth_sampler;
}

VkSampler TextureCache::GetOrCreateSampler(const VkSamplerCreateInfo& info) {
    // Bucket-vector lookup + samplerInfoEqual scan disambiguates collisions.
    // findOrCreateSampler returns a reference to the bucket entry by value
    // semantics (vvk::Sampler is move-only); we deref to a raw VkSampler.
    bool create_failed = false;
    auto creator       = [&](const VkSamplerCreateInfo& ci) -> vvk::Sampler {
        vvk::Sampler s;
        if (m_device.handle().CreateSampler(ci, s) != VK_SUCCESS) {
            LOG_ERROR("CreateSampler failed in TextureCache::GetOrCreateSampler");
            create_failed = true;
        }
        return s;
    };
    vvk::Sampler& entry = findOrCreateSampler(m_sampler_cache, info, creator);
    if (create_failed && *entry == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    return *entry;
}
