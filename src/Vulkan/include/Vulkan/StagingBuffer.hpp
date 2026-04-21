#pragma once
#include "Core/NoCopyMove.hpp"
#include "Instance.hpp"
#include "Parameters.hpp"
#include "vk_mem_alloc.h"

namespace wallpaper
{
namespace vulkan
{

class Device;
class StagingBuffer;

class StagingBufferRef {
public:
    VkDeviceSize size { 0 };
    VkDeviceSize offset { 0 };

    operator bool() const { return m_allocation != VK_NULL_HANDLE; }

private:
    friend class StagingBuffer;
    VmaVirtualAllocation m_allocation {};
    size_t               m_virtual_index { 0 };
};

class StagingBuffer : NoCopy, NoMove {
public:
    // slot_count > 1 gives N-way staging (for multi-frame-in-flight): the
    // GPU buffer stays single (shared, protected by queue ordering) but N
    // independent staging buffers let frame N+1 overwrite its staging
    // without racing frame N's pending vkCmdCopyBuffer.  setCurrentSlot()
    // picks which staging writeToBuf / recordUpload operate on.  init-only
    // writes (prepare-time defaults + zero-fill) must go to writeToBufAllSlots
    // / fillBuf so they survive slot switches.
    StagingBuffer(const Device&, VkDeviceSize size, VkBufferUsageFlags,
                  size_t slot_count = 1);
    ~StagingBuffer();

    bool allocate();
    void destroy();

    void setCurrentSlot(size_t slot);
    // Broadcast mode: while enabled, writeToBuf transparently mirrors to
    // every slot (so existing per-write code paths like UpdateUniform work
    // unmodified at prepare-time).  Disable before entering the render
    // loop so per-frame writes only hit the current slot.
    void setBroadcastMode(bool enabled) { m_broadcast = enabled; }

    bool allocateSubRef(VkDeviceSize size, StagingBufferRef&, VkDeviceSize alignment = 1);
    void unallocateSubRef(const StagingBufferRef&);
    bool writeToBuf(const StagingBufferRef&, std::span<uint8_t>, size_t offset = 0);
    // Broadcast a write to all slots — use for init-time/default uniform
    // seeding where the value is expected to persist across slot switches.
    bool writeToBufAllSlots(const StagingBufferRef&, std::span<uint8_t>, size_t offset = 0);
    bool fillBuf(const StagingBufferRef& ref, size_t offset, size_t size, uint8_t c);

    bool recordUpload(vvk::CommandBuffer&);

    VkBuffer gpuBuf() const;

private:
    struct VirtualBlock {
        VmaVirtualBlock handle {};
        bool            enabled { false };
        size_t          index { 0 };
        VkDeviceSize    offset { 0 };
        VkDeviceSize    size { 0 };
    };

    VkResult      mapStageBuf(size_t slot);
    VirtualBlock* newVirtualBlock(VkDeviceSize);
    bool          increaseBuf(VkDeviceSize);

    const Device& m_device;
    VkDeviceSize  m_size_step;

    VkBufferUsageFlags m_usage;

    size_t                           m_slot_count { 1 };
    size_t                           m_current_slot { 0 };
    bool                             m_broadcast { false };
    std::vector<void*>               m_stage_raws;
    std::vector<VmaBufferParameters> m_stage_bufs;
    std::vector<VirtualBlock>        m_virtual_blocks {};

    VmaBufferParameters m_gpu_buf;
};

} // namespace vulkan
} // namespace wallpaper
