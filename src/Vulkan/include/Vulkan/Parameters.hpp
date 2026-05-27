#pragma once

#include "Instance.hpp"
#include "Swapchain.hpp"
#include "Core/NoCopyMove.hpp"
#include "vk_mem_alloc.h"
#include "vvk/vma_wrapper.hpp"

namespace wallpaper
{
namespace vulkan
{

struct QueueParameters {
    vvk::Queue handle;
    uint32_t   family_index;
};

struct VmaBufferParameters {
    vvk::VmaBuffer handle;
    std::size_t    req_size;

    VmaBufferParameters();
    ~VmaBufferParameters();
    VmaBufferParameters(VmaBufferParameters&& o) noexcept;
    VmaBufferParameters& operator=(VmaBufferParameters&& o) noexcept;
};

struct BufferParameters {
    VkBuffer    handle;
    std::size_t req_size;
    BufferParameters()  = default;
    ~BufferParameters() = default;
    BufferParameters(const VmaBufferParameters& o) noexcept
        : handle(*o.handle), req_size(o.req_size) {}
};

struct VmaImageParameters : NoCopy {
    vvk::VmaImage  handle;
    vvk::ImageView view;       // multi-mip view (covers all levels) — for sampling
    vvk::ImageView mip0_view;  // single-mip view at level 0 — for render attachment.
                               // Only populated when mipmap_level > 1; for single-mip
                               // images `view` already covers exactly mip 0 and the
                               // ImageParameters wrapper falls back to it.
    // Non-owning raw VkSampler.  The owning vvk::Sampler lives in
    // TextureCache::m_sampler_cache (dedup'd by VkSamplerCreateInfo hash) and
    // is destroyed there in Clear() AFTER this image — which references the
    // sampler via descriptor sets — has been destroyed.
    VkSampler      sampler { VK_NULL_HANDLE };
    VkExtent3D     extent;
    uint           mipmap_level { 1 };
    // First-use barrier latch for the UNDEFINED -> initial-usage transition.
    // Owned here (not a global VkImage-keyed set) so the bool dies with the
    // VkImage handle — Vulkan is free to recycle handles after a free, and a
    // stale global tracker would skip the barrier for the recycled image
    // (validation: "Image must be in COLOR_ATTACHMENT_OPTIMAL but is in
    // UNDEFINED"; black tile / undefined contents on real GPUs).
    bool initial_layout_transitioned { false };

    VmaImageParameters();
    ~VmaImageParameters();
    VmaImageParameters(VmaImageParameters&& o) noexcept;
    VmaImageParameters& operator=(VmaImageParameters&& o) noexcept;
};

struct ExImageParameters : NoCopy {
    vvk::DeviceMemory    mem {};
    VkMemoryRequirements mem_reqs {};

    vvk::Image     handle;
    vvk::ImageView view;
    // Non-owning raw VkSampler.  Same dedup'd lifetime as VmaImageParameters
    // — TextureCache::m_sampler_cache owns the underlying vvk::Sampler.
    VkSampler      sampler { VK_NULL_HANDLE };
    VkExtent3D     extent;
    uint           mipmap_level { 1 };
    int            fd { 0 };

    ExImageParameters();
    ~ExImageParameters();
    ExImageParameters(ExImageParameters&& o) noexcept;
    ExImageParameters& operator=(ExImageParameters&& o) noexcept;
};

struct ImageParameters {
    VkImage     handle;
    VkImageView view;        // multi-mip view — for sampler binding
    VkImageView mip0_view;   // single-mip-level-0 view — for render attachment.
                             // Equals `view` when the source image is single-mip
                             // (mipmap_level == 1) so callers can use unconditionally.
    VkSampler   sampler;
    VkExtent3D  extent;
    uint        mipmap_level { 1 };

    ImageParameters()  = default;
    ~ImageParameters() = default;
    ImageParameters(const VmaImageParameters& o) noexcept
        : handle(*o.handle),
          view(*o.view),
          mip0_view(o.mipmap_level > 1 ? *o.mip0_view : *o.view),
          sampler(o.sampler),
          extent(o.extent),
          mipmap_level(o.mipmap_level) {}
    ImageParameters(const ExImageParameters& o) noexcept
        : handle(*o.handle),
          view(*o.view),
          mip0_view(*o.view),
          sampler(o.sampler),
          extent(o.extent),
          mipmap_level(o.mipmap_level) {}
};

struct ImageSlots : NoCopy {
    std::vector<VmaImageParameters> slots;

    ImageSlots();
    ~ImageSlots();
    ImageSlots(ImageSlots&& o) noexcept;
    ImageSlots& operator=(ImageSlots&& o) noexcept;
};

struct ImageSlotsRef {
    std::vector<ImageParameters> slots;

    idx active { 0 };

    auto& getActive() const {
        if (active > 0 && active >= std::ssize(slots)) return slots[0];
        return slots[(usize)active];
    }
    ImageSlotsRef();
    ~ImageSlotsRef();
    ImageSlotsRef(const ImageSlots&);
};

} // namespace vulkan
} // namespace wallpaper
