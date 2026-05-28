#pragma once
#include "Instance.hpp"
#include "Swapchain.hpp"
#include "vk_mem_alloc.h"
#include "Parameters.hpp"
#include "TextureCache.hpp"

#include <array>
#include <vector>

namespace wallpaper
{
namespace vulkan
{

namespace queue_selection
{
// First family that appears in BOTH lists, else UINT32_MAX.  Used by
// Device::ChooseDeviceQueue to pick a single graphics+present family — that
// keeps the command pool, render submit queue, and present queue on the same
// family so command buffers don't get submitted cross-family
// (VUID-vkQueueSubmit-pCommandBuffers-00074).  Pure function — unit-tested
// in test_DeviceQueueSelection.cpp.
uint32_t FindUnifiedFamily(const std::vector<uint32_t>& graphics,
                           const std::vector<uint32_t>& present);
} // namespace queue_selection

class PipelineParameters;

class Device : NoCopy, NoMove {
public:
    Device();
    ~Device();

    static bool Create(Instance&, std::span<const Extension> exts, VkExtent2D extent, Device&);
    static bool CheckGPU(vvk::PhysicalDevice gpu, std::span<const Extension> exts,
                         VkSurfaceKHR surface);

    void Destroy();

    const auto&           graphics_queue() const { return m_graphics_queue; }
    const auto&           present_queue() const { return m_present_queue; }
    const auto&           device() const { return m_device; }
    const auto&           handle() const { return m_device; }
    const auto&           gpu() const { return m_gpu; }
    // True iff this device advertises VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT on
    // VK_FORMAT_D32_SFLOAT for optimal tiling.  Probed once at Device::Create.
    // Consumers (volumetric chain, future SSAO/DOF) gate the depth-as-sampled
    // path on this; when false, they emit a depth-to-color resolve fallback.
    bool                  d32_sampleable() const { return m_d32_sampleable; }
    const auto&           limits() const { return m_limits; }
    float                 maxAnisotropy() const { return m_limits.maxSamplerAnisotropy; }
    VkSampleCountFlagBits maxMSAASamples() const {
        VkSampleCountFlags counts =
            m_limits.framebufferColorSampleCounts & m_limits.framebufferDepthSampleCounts;
        if (counts & VK_SAMPLE_COUNT_8_BIT) return VK_SAMPLE_COUNT_8_BIT;
        if (counts & VK_SAMPLE_COUNT_4_BIT) return VK_SAMPLE_COUNT_4_BIT;
        if (counts & VK_SAMPLE_COUNT_2_BIT) return VK_SAMPLE_COUNT_2_BIT;
        return VK_SAMPLE_COUNT_1_BIT;
    }
    const auto& vma_allocator() const { return *m_allocator; }
    const auto& cmd_pool() const { return m_command_pool; }
    // VkPipelineCache shared across all vkCreateGraphicsPipelines calls.
    // Loaded from $XDG_CACHE_HOME/wallpaper-scene-renderer/pipeline.cache at
    // Create() (header-validated against driver vendor/device/UUID) and
    // persisted back in Destroy().  Returns VK_NULL_HANDLE if creation
    // failed — vkCreateGraphicsPipelines accepts that fallback.
    VkPipelineCache pipeline_cache() const { return *m_pipeline_cache; }
    const auto& swapchain() const { return m_swapchain; }
    Swapchain&  mut_swapchain() { return m_swapchain; }
    const auto& out_extent() const { return m_extent; }
    void        set_out_extent(VkExtent2D v) { m_extent = v; }

    bool supportExt(std::string_view) const;

    TextureCache& tex_cache() const { return *m_tex_cache; }

    // Per-heap usage/budget snapshot reported by VMA.  When VK_EXT_memory_budget
    // is enabled (driver advertises it AND the VMA flag is set in Device::Create),
    // these numbers come from the driver and include all VRAM consumers
    // process-wide; otherwise they fall back to allocator-internal bookkeeping
    // which only counts bytes this allocator itself has minted.
    struct HeapBudget {
        VkDeviceSize      usage { 0 };
        VkDeviceSize      budget { 0 };
        VkMemoryHeapFlags flags { 0 };
    };
    std::array<HeapBudget, VK_MAX_MEMORY_HEAPS> GetHeapBudgets() const;

    VkDeviceSize GetUsage() const;

private:
    std::vector<VkDeviceQueueCreateInfo> ChooseDeviceQueue(VkSurfaceKHR = {});

    vvk::DeviceDispatch     dld;
    vvk::Device             m_device;
    vvk::PhysicalDevice     m_gpu;
    vvk::VmaAllocatorHandle m_allocator;

    VkPhysicalDeviceLimits m_limits;
    Set<std::string>       m_extensions;

    Swapchain m_swapchain;

    vvk::CommandPool    m_command_pool;
    vvk::PipelineCache  m_pipeline_cache;

    QueueParameters m_graphics_queue;
    QueueParameters m_present_queue;

    // output extent
    VkExtent2D m_extent { 1, 1 };

    // Probed in Device::Create from vkGetPhysicalDeviceFormatProperties on
    // VK_FORMAT_D32_SFLOAT; invariant for a given device.
    bool m_d32_sampleable { false };

    std::unique_ptr<TextureCache> m_tex_cache;
};

} // namespace vulkan
} // namespace wallpaper
