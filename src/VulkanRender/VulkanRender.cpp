#include "VulkanRender.hpp"
#include "Image.hpp"

#include "Utils/Logging.h"
#include "Utils/SceneProfiler.h"
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "Interface/IShaderValueUpdater.h"

#include "Utils/Algorism.h"

#include <glslang/Public/ShaderLang.h>
#include <unordered_set>
#include <chrono>
#include <cstdio>

#include "Vulkan/Device.hpp"
#include "Vulkan/TextureCache.hpp"
#include "Vulkan/Swapchain.hpp"
#include "Vulkan/VulkanExSwapchain.hpp"

#include "VulkanPass.hpp"
#include "PrePass.hpp"
#include "FinPass.hpp"
#include "CustomShaderPass.hpp"
#include "Resource.hpp"

#include "Core/ArrayHelper.hpp"

#include <cassert>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <mutex>
#include <atomic>
#include <cstdio>

#if ENABLE_RENDERDOC_API
#    include "RenderDoc.h"
#endif

using namespace wallpaper::vulkan;

constexpr uint64_t vk_wait_time { 10u * 1000u * 1000000u };
// 2 frames in flight: lets CPU record frame N+1 while GPU executes frame N,
// collapsing drawFrame wallclock from (CPU + GPU) to max(CPU, GPU) at
// steady state.  Per-slot resources: command buffer, fence, semaphore pair,
// and a staging slot inside m_dyn_buf (GPU buffer stays shared — intra-
// queue ordering serialises the copies).
constexpr uint32_t kFramesInFlight = 2;
// 1 one-shot upload cmd (vertex_buf seeding at compile) + kFramesInFlight
// render cmds (one per in-flight slot).
constexpr uint32_t vk_command_num { 1 + kFramesInFlight };

// Like VVK_CHECK_VOID_RE but also sets m_device_lost on VK_ERROR_DEVICE_LOST
#define VVK_CHECK_DEVICE_LOST(f)                                  \
    {                                                             \
        VkResult _res = (f);                                      \
        if (_res != VK_SUCCESS && _res != VK_SUBOPTIMAL_KHR) {    \
            LOG_ERROR("VkResult is \"%s\"", vvk::ToString(_res)); \
            if (_res == VK_ERROR_DEVICE_LOST) {                   \
                m_device_lost = true;                             \
            }                                                     \
            return;                                               \
        }                                                         \
    }

constexpr std::array base_inst_exts {
    Extension { false, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME },
};
constexpr std::array base_device_exts {
    Extension { false, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
    Extension { true, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME },
    Extension { true, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME }
};

struct VulkanRender::Impl {
    Impl()  = default;
    ~Impl() = default;

    bool init(RenderInitInfo);
    void destroy();
    bool hdrOutput() const { return m_hdr_output; }
    bool hdrContent() const { return m_hdr_content; }

    void drawFrame(Scene&);

    bool CreateRenderingResource(RenderingResources&, vvk::CommandBuffer cmd);
    void DestroyRenderingResource(RenderingResources&);

    void clearLastRenderGraph(Scene* scene);
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void UpdateCameraFillMode(Scene&, wallpaper::FillMode);

    bool initRes();
    void drawFrameSwapchain();
    void drawFrameOffscreen();
    void setRenderTargetSize(Scene&, rg::RenderGraph&);

    Instance                m_instance;
    std::unique_ptr<Device> m_device;

    std::unique_ptr<PrePass> m_prepass { nullptr };
    std::unique_ptr<FinPass> m_finpass { nullptr };

    std::unique_ptr<FinPass> m_testpass { nullptr };
    ReDrawCB                 m_redraw_cb;

    std::unique_ptr<StagingBuffer> m_vertex_buf { nullptr };
    std::unique_ptr<StagingBuffer> m_dyn_buf { nullptr };

    vvk::CommandBuffers                                m_cmds;
    vvk::CommandBuffer                                 m_upload_cmd;
    std::array<vvk::CommandBuffer, kFramesInFlight>    m_render_cmds;

    bool m_with_surface { false };
    bool m_inited { false };
    bool m_pass_loaded { false };
    bool m_device_lost { false };
    bool m_hdr_output { false };
    bool m_hdr_content { false };

    std::unique_ptr<VulkanExSwapchain>                     m_ex_swapchain;
    std::array<RenderingResources, kFramesInFlight>        m_rendering_resources;
    uint64_t                                               m_frame_index { 0 };

    // Swapchain synchronization semaphores — one pair per in-flight slot.
    std::array<vvk::Semaphore, kFramesInFlight> m_sem_image_available;
    std::array<vvk::Semaphore, kFramesInFlight> m_sem_render_finished;

    std::vector<VulkanPass*> m_passes;

    // Screenshot request.  Set from any thread via setScreenshotPath;
    // consumed on the render thread inside drawFrameSwapchain after WaitIdle.
    std::mutex        m_screenshot_mutex;
    std::string       m_pending_screenshot_path;
    std::atomic<bool> m_screenshot_done { false };

    // Per-pass RT dump request.  When set, after the next frame's WaitIdle
    // we iterate every CustomShaderPass, read back its output image, and
    // write a PPM file.  Drops the dir on completion so repeat captures
    // need a fresh set-call.
    std::mutex        m_pass_dump_mutex;
    std::string       m_pending_pass_dump_dir;
    std::atomic<bool> m_pass_dump_done { false };

    void takeScreenshotIfRequested(VkImage swap_image, VkFormat swap_format, uint32_t width,
                                   uint32_t height);
    void dumpPassesIfRequested();
};

// Write a swapchain image to a PPM file.  Called with the device idle.
// Returns true on success.  Keeps a small code footprint — PPM is a trivial
// header-plus-RGB format, avoids pulling in a PNG encoder.
static bool writeSwapchainToPPM(Device& device, VkImage swap_image, VkFormat swap_format,
                                uint32_t width, uint32_t height, const std::string& path) {
    const VkDeviceSize bufferSize = (VkDeviceSize)width * height * 4;

    // 1) Host-visible staging buffer that we can map after the copy.
    VmaBufferParameters staging;
    staging.req_size = bufferSize;
    {
        VkBufferCreateInfo ci {
            .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .size                  = bufferSize,
            .usage                 = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
        };
        VmaAllocationCreateInfo vma_info = {};
        vma_info.usage                   = VMA_MEMORY_USAGE_CPU_ONLY;
        if (vvk::CreateBuffer(device.vma_allocator(), ci, vma_info, staging.handle) != VK_SUCCESS) {
            LOG_ERROR("Screenshot: failed to create staging buffer");
            return false;
        }
    }

    // 2) Allocate a one-shot command buffer.  CommandPool::Allocate returns
    //    VkResult (VK_SUCCESS == 0), so check against VK_SUCCESS rather than
    //    the implicit-bool convert that would invert the meaning.
    vvk::CommandBuffers cmds;
    if (device.cmd_pool().Allocate(1, VK_COMMAND_BUFFER_LEVEL_PRIMARY, cmds) != VK_SUCCESS) {
        LOG_ERROR("Screenshot: failed to allocate command buffer");
        return false;
    }
    vvk::CommandBuffer cmd(cmds[0], device.handle().Dispatch());

    // 3) Record: transition swapchain image to TRANSFER_SRC, copy to buffer, back to PRESENT_SRC.
    VkCommandBufferBeginInfo begin {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (cmd.Begin(begin) != VK_SUCCESS) {
        LOG_ERROR("Screenshot: cmd Begin failed");
        return false;
    }

    VkImageMemoryBarrier toSrc {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext               = nullptr,
        .srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = swap_image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, toSrc);

    VkBufferImageCopy region {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset       = { 0, 0, 0 },
        .imageExtent       = { width, height, 1 },
    };
    cmd.CopyImageToBuffer(
        swap_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *staging.handle, region);

    VkImageMemoryBarrier toPresent = toSrc;
    toPresent.srcAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask        = VK_ACCESS_MEMORY_READ_BIT;
    toPresent.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout            = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    cmd.PipelineBarrier(
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, toPresent);

    if (cmd.End() != VK_SUCCESS) {
        LOG_ERROR("Screenshot: cmd End failed");
        return false;
    }

    // 4) Submit and wait.  Using graphics queue — same one the swapchain image
    //    belongs to, so no queue ownership transfer needed.
    VkSubmitInfo sub {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = nullptr,
        .waitSemaphoreCount   = 0,
        .pWaitSemaphores      = nullptr,
        .pWaitDstStageMask    = nullptr,
        .commandBufferCount   = 1,
        .pCommandBuffers      = cmd.address(),
        .signalSemaphoreCount = 0,
        .pSignalSemaphores    = nullptr,
    };
    if (device.graphics_queue().handle.Submit(sub, {}) != VK_SUCCESS) {
        LOG_ERROR("Screenshot: queue Submit failed");
        return false;
    }
    if (device.graphics_queue().handle.WaitIdle() != VK_SUCCESS) {
        LOG_ERROR("Screenshot: queue WaitIdle failed");
        return false;
    }

    // 5) Map memory, write PPM header + RGB rows.  Swapchain formats in this
    //    codebase are BGRA8_UNORM or RGBA8_UNORM (see chooseSwapSurfaceFormat).
    void* mapped = nullptr;
    if (staging.handle.MapMemory(&mapped) != VK_SUCCESS) {
        LOG_ERROR("Screenshot: map failed");
        return false;
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (! f) {
        LOG_ERROR("Screenshot: cannot open '%s' for write", path.c_str());
        staging.handle.UnMapMemory();
        return false;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);

    const bool bgr =
        (swap_format == VK_FORMAT_B8G8R8A8_UNORM || swap_format == VK_FORMAT_B8G8R8A8_SRGB);
    const auto*          pixels = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* src = pixels + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            if (bgr) {
                row[x * 3 + 0] = src[x * 4 + 2];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + 0];
            } else {
                row[x * 3 + 0] = src[x * 4 + 0];
                row[x * 3 + 1] = src[x * 4 + 1];
                row[x * 3 + 2] = src[x * 4 + 2];
            }
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    staging.handle.UnMapMemory();

    LOG_INFO("Screenshot saved: %s (%ux%u)", path.c_str(), width, height);
    return true;
}

void VulkanRender::Impl::takeScreenshotIfRequested(VkImage swap_image, VkFormat swap_format,
                                                   uint32_t width, uint32_t height) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(m_screenshot_mutex);
        path = std::move(m_pending_screenshot_path);
        m_pending_screenshot_path.clear();
    }
    if (path.empty()) return;
    writeSwapchainToPPM(*m_device, swap_image, swap_format, width, height, path);
    m_screenshot_done.store(true, std::memory_order_release);
}

// Generic image readback for per-pass dump.  The src image is assumed to be
// in SHADER_READ_ONLY_OPTIMAL layout — that's where intermediate RTs land
// after their render pass's finalLayout transition, because the render graph
// uses them as shader inputs for downstream passes.  Returns true on success.
// Format is inferred as RGBA8 UNORM for simplicity; HDR (RGBA16F) RTs produce
// a garbled PPM but never crash.
static bool writeImageToPPM(Device& device, VkImage image, VkImageLayout current_layout,
                            uint32_t width, uint32_t height, const std::string& path) {
    if (image == VK_NULL_HANDLE || width == 0 || height == 0) return false;
    const VkDeviceSize bufferSize = (VkDeviceSize)width * height * 4;

    VmaBufferParameters staging;
    staging.req_size = bufferSize;
    {
        VkBufferCreateInfo ci {
            .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .size                  = bufferSize,
            .usage                 = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
        };
        VmaAllocationCreateInfo vma_info = {};
        vma_info.usage                   = VMA_MEMORY_USAGE_CPU_ONLY;
        if (vvk::CreateBuffer(device.vma_allocator(), ci, vma_info, staging.handle) != VK_SUCCESS)
            return false;
    }

    vvk::CommandBuffers cmds;
    if (device.cmd_pool().Allocate(1, VK_COMMAND_BUFFER_LEVEL_PRIMARY, cmds) != VK_SUCCESS)
        return false;
    vvk::CommandBuffer cmd(cmds[0], device.handle().Dispatch());

    VkCommandBufferBeginInfo begin {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    if (cmd.Begin(begin) != VK_SUCCESS) return false;

    VkImageMemoryBarrier toSrc {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext               = nullptr,
        .srcAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = current_layout,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    cmd.PipelineBarrier(
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, toSrc);

    VkBufferImageCopy region {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset       = { 0, 0, 0 },
        .imageExtent       = { width, height, 1 },
    };
    cmd.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *staging.handle, region);

    VkImageMemoryBarrier toBack = toSrc;
    toBack.srcAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
    toBack.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
    toBack.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toBack.newLayout            = current_layout;
    cmd.PipelineBarrier(
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, toBack);

    if (cmd.End() != VK_SUCCESS) return false;

    VkSubmitInfo sub {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = nullptr,
        .waitSemaphoreCount   = 0,
        .pWaitSemaphores      = nullptr,
        .pWaitDstStageMask    = nullptr,
        .commandBufferCount   = 1,
        .pCommandBuffers      = cmd.address(),
        .signalSemaphoreCount = 0,
        .pSignalSemaphores    = nullptr,
    };
    if (device.graphics_queue().handle.Submit(sub, {}) != VK_SUCCESS) return false;
    if (device.graphics_queue().handle.WaitIdle() != VK_SUCCESS) return false;

    void* mapped = nullptr;
    if (staging.handle.MapMemory(&mapped) != VK_SUCCESS) return false;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (! f) {
        staging.handle.UnMapMemory();
        return false;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);
    const auto*          pixels = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* src = pixels + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; x++) {
            // RTs are typically RGBA8; swap for BGRA would be detectable from
            // the render target format, but we don't track that here.
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    staging.handle.UnMapMemory();
    return true;
}

// Per-pass dump state.  CustomShaderPass::execute pushes an entry + records
// the image→staging copy into the current cmd buffer.  After the frame is
// submitted and idle, dumpPassesIfRequested() maps each staging buffer and
// writes a PPM.  Using raw globals (extern) because VulkanPass doesn't have
// a context object we can thread state through, and adding one would touch
// every pass subclass for a debug-only feature.
namespace wallpaper
{
namespace vulkan
{
struct PassDumpEntry {
    VmaBufferParameters staging;
    uint32_t            width;
    uint32_t            height;
    std::string         shader;
    std::string         output;
    int32_t             node_id;
    size_t              pass_index;
};
bool                        g_pass_dump_active  = false;
std::vector<PassDumpEntry>* g_pass_dump_entries = nullptr;
Device const*               g_pass_dump_device  = nullptr;
static size_t               g_pass_dump_counter = 0;

void g_pass_dump_record(const vvk::CommandBuffer& cmd, VkImage image, uint32_t w, uint32_t h,
                        const std::string& shader, const std::string& output, int32_t node_id) {
    if (! g_pass_dump_active || ! g_pass_dump_entries || ! g_pass_dump_device) return;
    if (image == VK_NULL_HANDLE || w == 0 || h == 0) return;
    // Skip enormous RTs — dumping a scene's full 4K background uses ~66MB
    // per pass, and cumulatively the ~200 passes exhaust VMA host memory so
    // MapMemory hangs mid-loop.  8 MP covers character layers up to 2252x2306
    // while still keeping the 4K (8.3 MP) scene-wide RTs out.
    constexpr uint64_t kMaxPixels = 8ull * 1024 * 1024;
    if ((uint64_t)w * h > kMaxPixels) {
        static int s_skipped = 0;
        if (++s_skipped <= 10) {
            LOG_INFO("pass dump: skip idx=%zu id=%d %s -> %s (%ux%u, too large)",
                     g_pass_dump_counter,
                     node_id,
                     shader.c_str(),
                     output.c_str(),
                     w,
                     h);
        }
        g_pass_dump_counter++;
        return;
    }
    LOG_INFO("pass dump: record idx=%zu id=%d %s -> %s (%ux%u)",
             g_pass_dump_counter,
             node_id,
             shader.c_str(),
             output.c_str(),
             w,
             h);

    const VkDeviceSize bufferSize = (VkDeviceSize)w * h * 4;
    PassDumpEntry      entry {};
    entry.width            = w;
    entry.height           = h;
    entry.shader           = shader;
    entry.output           = output;
    entry.node_id          = node_id;
    entry.pass_index       = g_pass_dump_counter++;
    entry.staging.req_size = bufferSize;

    VkBufferCreateInfo ci {
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = bufferSize,
        .usage                 = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr,
    };
    VmaAllocationCreateInfo vma_info = {};
    vma_info.usage                   = VMA_MEMORY_USAGE_CPU_ONLY;
    if (vvk::CreateBuffer(
            g_pass_dump_device->vma_allocator(), ci, vma_info, entry.staging.handle) !=
        VK_SUCCESS) {
        LOG_ERROR("pass dump: staging alloc failed");
        return;
    }

    // At this point in CustomShaderPass::execute, EndRenderPass has just
    // fired.  The render pass's finalLayout was SHADER_READ_ONLY_OPTIMAL
    // (for intermediate RTs) or COLOR_ATTACHMENT_OPTIMAL (some passes);
    // we use ALL_COMMANDS → TRANSFER pipeline barrier with the old layout
    // marked UNDEFINED would discard data, so we accept SHADER_READ_ONLY
    // here and the Vulkan runtime will complain loudly if we're wrong.
    VkImageMemoryBarrier toSrc {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext               = nullptr,
        .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    cmd.PipelineBarrier(
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, toSrc);

    VkBufferImageCopy region {
        .bufferOffset      = 0,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset       = { 0, 0, 0 },
        .imageExtent       = { w, h, 1 },
    };
    cmd.CopyImageToBuffer(
        image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *entry.staging.handle, region);

    VkImageMemoryBarrier toBack = toSrc;
    toBack.srcAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
    toBack.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
    toBack.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toBack.newLayout            = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    cmd.PipelineBarrier(
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, toBack);

    g_pass_dump_entries->push_back(std::move(entry));
}
} // namespace vulkan
} // namespace wallpaper

void VulkanRender::Impl::dumpPassesIfRequested() {
    // Two phases.  On a newly requested dump, we arm the globals so the
    // NEXT frame's pass executes record copies into staging.  On the
    // following frame (if entries exist), we unmap+write+reset.
    std::string dir;
    {
        std::lock_guard<std::mutex> lk(m_pass_dump_mutex);
        dir = m_pending_pass_dump_dir;
    }

    static std::string                                   s_dump_dir;
    static std::vector<wallpaper::vulkan::PassDumpEntry> s_entries;
    static int                                           s_phase = 0; // 0 idle, 1 armed

    using wallpaper::vulkan::g_pass_dump_active;
    using wallpaper::vulkan::g_pass_dump_counter;
    using wallpaper::vulkan::g_pass_dump_device;
    using wallpaper::vulkan::g_pass_dump_entries;

    if (s_phase == 0 && ! dir.empty()) {
        s_dump_dir = dir;
        s_entries.clear();
        wallpaper::vulkan::g_pass_dump_counter = 0;
        wallpaper::vulkan::g_pass_dump_entries = &s_entries;
        wallpaper::vulkan::g_pass_dump_device  = m_device.get();
        wallpaper::vulkan::g_pass_dump_active  = true;
        s_phase                                = 1;
        LOG_INFO("pass dump: armed for next frame (dir=%s)", s_dump_dir.c_str());
        return;
    }

    if (s_phase == 1) {
        // Frame completed with copies recorded.  Write PPMs now.
        wallpaper::vulkan::g_pass_dump_active = false;

        std::error_code ec;
        std::filesystem::create_directories(s_dump_dir, ec);

        auto sanitize = [](std::string s) {
            for (auto& c : s)
                if (c == '/' || c == '\\' || c == ':') c = '_';
            if (s.size() > 64) s.resize(64);
            return s;
        };

        size_t written = 0;
        for (auto& entry : s_entries) {
            void* mapped = nullptr;
            if (entry.staging.handle.MapMemory(&mapped) != VK_SUCCESS) continue;

            char name_buf[64];
            std::snprintf(
                name_buf, sizeof(name_buf), "pass_%04zu_id%d", entry.pass_index, entry.node_id);
            std::string path = s_dump_dir + "/" + name_buf + "_" + sanitize(entry.shader) + "_" +
                               sanitize(entry.output) + ".ppm";

            FILE* f = std::fopen(path.c_str(), "wb");
            if (f) {
                std::fprintf(f, "P6\n%u %u\n255\n", entry.width, entry.height);
                const auto*          px = static_cast<const uint8_t*>(mapped);
                std::vector<uint8_t> row((size_t)entry.width * 3);
                for (uint32_t y = 0; y < entry.height; y++) {
                    const uint8_t* src = px + (size_t)y * entry.width * 4;
                    for (uint32_t x = 0; x < entry.width; x++) {
                        row[x * 3 + 0] = src[x * 4 + 0];
                        row[x * 3 + 1] = src[x * 4 + 1];
                        row[x * 3 + 2] = src[x * 4 + 2];
                    }
                    std::fwrite(row.data(), 1, row.size(), f);
                }
                std::fclose(f);
                written++;
            }
            entry.staging.handle.UnMapMemory();
        }
        LOG_INFO("pass dump: wrote %zu PPMs -> %s", written, s_dump_dir.c_str());
        s_entries.clear();
        wallpaper::vulkan::g_pass_dump_entries = nullptr;
        wallpaper::vulkan::g_pass_dump_device  = nullptr;
        s_phase                                = 0;
        {
            std::lock_guard<std::mutex> lk(m_pass_dump_mutex);
            m_pending_pass_dump_dir.clear();
        }
        m_pass_dump_done.store(true, std::memory_order_release);
    }
}

VulkanRender::VulkanRender(): pImpl(std::make_unique<Impl>()) {}
VulkanRender::~VulkanRender() {};

bool VulkanRender::inited() const { return pImpl->m_inited; }
bool VulkanRender::deviceLost() const { return pImpl->m_device_lost; }
bool VulkanRender::hdrContent() const { return pImpl->m_hdr_content; }
void VulkanRender::setSceneHdrContent(bool hdr) {
    if (! pImpl->m_finpass) return;
    if (pImpl->m_finpass->hdrContent() == hdr) return;
    pImpl->m_finpass->setHdrContent(hdr);
    pImpl->m_finpass->markNeedsReprepare();
}

bool VulkanRender::init(RenderInitInfo info) { return pImpl->init(info); }
void VulkanRender::destroy() { pImpl->destroy(); }
void VulkanRender::drawFrame(Scene& scene) { pImpl->drawFrame(scene); };
bool VulkanRender::reuploadTexture(const std::string& key, Image& image) {
    if (! pImpl->m_inited || ! pImpl->m_device) return false;
    return pImpl->m_device->tex_cache().ReuploadTex(key, image);
}
void VulkanRender::clearLastRenderGraph(Scene* scene) { pImpl->clearLastRenderGraph(scene); };
void VulkanRender::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    pImpl->compileRenderGraph(scene, rg);
};
void VulkanRender::UpdateCameraFillMode(Scene& scene, wallpaper::FillMode fill) {
    pImpl->UpdateCameraFillMode(scene, fill);
};

wallpaper::ExSwapchain* VulkanRender::exSwapchain() const { return pImpl->m_ex_swapchain.get(); };

void VulkanRender::setScreenshotPath(const std::string& path) {
    std::lock_guard<std::mutex> lk(pImpl->m_screenshot_mutex);
    pImpl->m_pending_screenshot_path = path;
    pImpl->m_screenshot_done.store(false, std::memory_order_release);
}
bool VulkanRender::screenshotDone() const {
    return pImpl->m_screenshot_done.load(std::memory_order_acquire);
}

void VulkanRender::setPassDumpDir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(pImpl->m_pass_dump_mutex);
    pImpl->m_pending_pass_dump_dir = dir;
    pImpl->m_pass_dump_done.store(false, std::memory_order_release);
}
bool VulkanRender::passDumpDone() const {
    return pImpl->m_pass_dump_done.load(std::memory_order_acquire);
}

bool VulkanRender::Impl::init(RenderInitInfo info) {
    if (m_inited) return true;

    m_redraw_cb = info.redraw_callback;
    VkExtent2D extent { info.width, info.height };
    if (extent.width * extent.height < 500 * 500) {
        LOG_ERROR("too small swapchain image size: %dx%d", extent.width, extent.height);
    } else {
        LOG_INFO("set swapchain image size: %dx%d", extent.width, extent.height);
    }

    std::vector<Extension> inst_exts { base_inst_exts.begin(), base_inst_exts.end() };
    std::vector<Extension> device_exts { base_device_exts.begin(), base_device_exts.end() };

    if (! info.offscreen) {
        std::transform(info.surface_info.instanceExts.begin(),
                       info.surface_info.instanceExts.end(),
                       std::back_inserter(inst_exts),
                       [](const auto& s) {
                           return Extension { true, s.c_str() };
                       });
        device_exts.push_back({ true, VK_KHR_SWAPCHAIN_EXTENSION_NAME });
    }

    std::vector<InstanceLayer> inst_layers;
    // valid layer
    if (info.enable_valid_layer) {
        inst_layers.push_back({ true, VALIDATION_LAYER_NAME });
        LOG_INFO("vulkan valid layer \"%s\" enabled", VALIDATION_LAYER_NAME.data());
    }

    if (! Instance::Create(m_instance, inst_exts, inst_layers)) {
        LOG_ERROR("init vulkan failed");
        return false;
    }
    if (! info.offscreen) {
        VkSurfaceKHR surface;
        VVK_CHECK_ACT(
            {
                LOG_ERROR("create vulkan surface failed");
                return false;
            },
            info.surface_info.createSurfaceOp(*m_instance.inst(), &surface));
        m_instance.setSurface(VkSurfaceKHR(surface));
        m_with_surface = true;
    }
    {
        auto surface   = *m_instance.surface();
        auto check_gpu = [&device_exts, surface](const vvk::PhysicalDevice& gpu) {
            return Device::CheckGPU(gpu, device_exts, surface);
        };
        if (! m_instance.ChoosePhysicalDevice(check_gpu, info.uuid)) return false;
    }

    {
        m_device = std::make_unique<Device>();
        if (! Device::Create(m_instance, device_exts, extent, *m_device)) {
            LOG_ERROR("init vulkan device failed");
            return false;
        }
    }

    if (info.offscreen) {
        VkFormat ex_fmt =
            info.hdr_output ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        m_ex_swapchain = CreateExSwapchain(*m_device,
                                           extent.width,
                                           extent.height,
                                           (info.offscreen_tiling == TexTiling::OPTIMAL
                                                ? VK_IMAGE_TILING_OPTIMAL
                                                : VK_IMAGE_TILING_LINEAR),
                                           ex_fmt);
        m_with_surface = false;
    }

    m_hdr_output  = info.hdr_output;
    m_hdr_content = info.hdr_content || info.hdr_output; // HDR output implies HDR content

    if (! initRes()) return false;

    // Initialize glslang once at startup
    glslang::InitializeProcess();

    m_inited = true;
    return m_inited;
}

bool VulkanRender::Impl::initRes() {
    m_prepass = std::make_unique<PrePass>(PrePass::Desc {});
    m_finpass = std::make_unique<FinPass>(FinPass::Desc {});
    m_finpass->setHdrPassthrough(m_hdr_output);
    m_finpass->setHdrContent(m_hdr_content);
    if (m_with_surface) {
        m_finpass->setPresentFormat(m_device->swapchain().format());
        m_finpass->setPresentQueueIndex(m_device->present_queue().family_index);
        m_finpass->setPresentLayout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        m_finpass->setPresentFormat(m_ex_swapchain->format());
        m_finpass->setPresentLayout(VK_IMAGE_LAYOUT_GENERAL);
        m_finpass->setPresentQueueIndex(VK_QUEUE_FAMILY_EXTERNAL);
    }
    /*
    m_testpass = std::make_unique<FinPass>(FinPass::Desc{});
    m_testpass->setPresentFormat(m_ex_swapchain->format());
    m_testpass->setPresentQueueIndex(m_device->graphics_queue().family_index);
    m_testpass->setPresentLayout(vk::ImageLayout::ePresentSrcKHR);
    */

    m_vertex_buf = std::make_unique<StagingBuffer>(*m_device,
                                                   2 * 1024 * 1024,
                                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m_dyn_buf    = std::make_unique<StagingBuffer>(*m_device,
                                                2 * 1024 * 1024,
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                kFramesInFlight);
    if (! m_vertex_buf->allocate()) return false;
    if (! m_dyn_buf->allocate()) return false;
    {
        auto& pool = m_device->cmd_pool();
        VVK_CHECK_BOOL_RE(pool.Allocate(vk_command_num, VK_COMMAND_BUFFER_LEVEL_PRIMARY, m_cmds));
        m_upload_cmd = vvk::CommandBuffer(m_cmds[0], m_device->handle().Dispatch());
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            m_render_cmds[i] = vvk::CommandBuffer(m_cmds[1 + i], m_device->handle().Dispatch());
        }
    }
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (! CreateRenderingResource(m_rendering_resources[i], m_render_cmds[i])) return false;
    }

#if ENABLE_RENDERDOC_API
    load_renderdoc_api();
#endif
    return true;
}

void VulkanRender::Impl::destroy() {
    if (! m_inited) return;

    // Finalize glslang (paired with InitializeProcess in init)
    glslang::FinalizeProcess();

    if (m_device && m_device->handle()) {
        VVK_CHECK(m_device->handle().WaitIdle());

        // res — destroy uses slot 0's RenderingResources for fixed members
        // (command ref, staging pointers); per-slot fences/semaphores are
        // released individually below.
        for (auto& p : m_passes) {
            p->destory(*m_device, m_rendering_resources[0]);
        }
        m_vertex_buf->destroy();
        m_dyn_buf->destroy();

        // Release sync objects before destroying device
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            m_sem_image_available[i].reset();
            m_sem_render_finished[i].reset();
            m_rendering_resources[i].fence_frame.reset();
        }

        m_device->Destroy();
    }
    m_instance.Destroy();
}

bool VulkanRender::Impl::CreateRenderingResource(RenderingResources& rr, vvk::CommandBuffer cmd) {
    rr.command = cmd;
    // Fences start signaled so the first frame can wait on them with zero
    // latency (no "first pass" branch in drawFrame).
    VVK_CHECK_BOOL_RE(m_device->handle().CreateFence(
        VkFenceCreateInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        },
        rr.fence_frame));

    // Which slot in m_sem_{image_available,render_finished} this RR maps
    // to — use address arithmetic so the mapping stays correct regardless
    // of how the array was passed in.
    const size_t slot = size_t(&rr - &m_rendering_resources[0]);
    if (m_with_surface && slot < kFramesInFlight) {
        VkSemaphoreCreateInfo sem_ci { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                       .pNext = nullptr };
        VVK_CHECK_BOOL_RE(
            m_device->handle().CreateSemaphore(sem_ci, m_sem_image_available[slot]));
        VVK_CHECK_BOOL_RE(
            m_device->handle().CreateSemaphore(sem_ci, m_sem_render_finished[slot]));
    }

    rr.vertex_buf = m_vertex_buf.get();
    rr.dyn_buf    = m_dyn_buf.get();
    return true;
}

void VulkanRender::Impl::DestroyRenderingResource(RenderingResources& rr) {}

// VulkanExSwapchain* VulkanRender::exSwapchain() const { return m_ex_swapchain.get(); }

void VulkanRender::Impl::drawFrame(Scene& scene) {
    WEK_PROFILE_SCOPE("VulkanRender::drawFrame");
    if (! (m_inited && m_pass_loaded)) return;

    // Periodic diagnostics: frame time + VMA usage + process RSS
    {
        static int    s_diag_frame   = 0;
        static auto   s_diag_start   = std::chrono::steady_clock::now();
        static auto   s_last_frame   = s_diag_start;
        static double s_frame_sum_ms = 0;
        static double s_frame_max_ms = 0;
        static int    s_frame_count  = 0;

        auto   now      = std::chrono::steady_clock::now();
        double frame_ms = std::chrono::duration<double, std::milli>(now - s_last_frame).count();
        s_last_frame    = now;
        s_frame_sum_ms += frame_ms;
        if (frame_ms > s_frame_max_ms) s_frame_max_ms = frame_ms;
        s_frame_count++;

        if (++s_diag_frame % 1800 == 0) { // every ~60s at 30fps
            double elapsed_s = std::chrono::duration<double>(now - s_diag_start).count();
            double avg_ms    = s_frame_count > 0 ? s_frame_sum_ms / s_frame_count : 0;
            double vma_mb    = m_device->GetUsage() / (1024.0 * 1024.0);

            // Read process RSS from /proc/self/statm
            long rss_pages = 0;
            if (FILE* f = fopen("/proc/self/statm", "r")) {
                long dummy;
                fscanf(f, "%ld %ld", &dummy, &rss_pages);
                fclose(f);
            }
            double rss_mb = rss_pages * 4096.0 / (1024.0 * 1024.0);

            extern int g_cache_hits;
            extern int g_cache_misses;
            int        hits   = g_cache_hits;
            int        misses = g_cache_misses;
            double     hit_pct =
                (hits + misses) > 0 ? 100.0 * hits / double(hits + misses) : 0.0;
            LOG_INFO("DIAG t=%.0fs frame=%d avg=%.1fms max=%.1fms VMA=%.1fMB RSS=%.1fMB "
                     "cache=%d/%d (%.0f%% skip)",
                     elapsed_s,
                     s_diag_frame,
                     avg_ms,
                     s_frame_max_ms,
                     vma_mb,
                     rss_mb,
                     hits,
                     hits + misses,
                     hit_pct);
            g_cache_hits   = 0;
            g_cache_misses = 0;
            s_frame_sum_ms = 0;
            s_frame_max_ms = 0;
            s_frame_count  = 0;
        }
    }

#if ENABLE_RENDERDOC_API
    if (rdoc_api)
        rdoc_api->StartFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE((VkInstance)m_instance.inst()), NULL);
#endif

    if (m_instance.offscreen()) {
        drawFrameOffscreen();
    } else {
        drawFrameSwapchain();
    }

    if (m_redraw_cb) m_redraw_cb();

#if ENABLE_RENDERDOC_API
    if (rdoc_api)
        rdoc_api->EndFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE((VkInstance)m_instance.inst()), NULL);
#endif
}

void VulkanRender::Impl::drawFrameSwapchain() {
    WEK_PROFILE_SCOPE("VulkanRender::drawFrameSwapchain");
    const size_t        slot = m_frame_index % kFramesInFlight;
    RenderingResources& rr   = m_rendering_resources[slot];

    // Wait for the slot's previous in-flight submission (two frames ago)
    // to complete.  This is the new "GPU done" barrier — moved from end
    // of the previous frame so frame N+1's CPU record can overlap with
    // frame N's GPU execution.
    VVK_CHECK_DEVICE_LOST(rr.fence_frame.Wait(vk_wait_time));
    VVK_CHECK_DEVICE_LOST(rr.fence_frame.Reset());

    // Point m_dyn_buf's staging writes at this slot before we begin
    // recording.  update_op lambdas called during execute() will hit the
    // selected slot; recordUpload / gpuBuf likewise.
    m_dyn_buf->setCurrentSlot(slot);

    uint32_t image_index = 0;
    {
        VVK_CHECK_VOID_RE(m_device->handle().AcquireNextImageKHR(*m_device->swapchain().handle(),
                                                                 vk_wait_time,
                                                                 *m_sem_image_available[slot],
                                                                 {},
                                                                 &image_index));
    }
    const auto& image = m_device->swapchain().images()[image_index];

    m_finpass->setPresent(image);

    (void)rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    m_dyn_buf->recordUpload(rr.command);
    {
        static bool _dumped             = false;
        static int  _render_frame_count = 0;
        int         prepared_count = 0, skipped_count = 0;
        // Reset per-frame exec pass counter (extern from CustomShaderPass)
        extern int                         g_exec_pass_counter;
        extern int                         g_exec_frame_counter;
        extern std::unordered_set<VkImage> g_depth_inited_frame;
        g_exec_pass_counter  = 0;
        g_exec_frame_counter = _render_frame_count;
        g_depth_inited_frame.clear();
        for (auto* p : m_passes) {
            if (p->prepared()) {
                p->execute(*m_device, rr);
                prepared_count++;
            } else {
                skipped_count++;
            }
        }
        if (! _dumped) {
            LOG_INFO("render frame: %d passes executed, %d skipped (not prepared), %zu total",
                     prepared_count,
                     skipped_count,
                     m_passes.size());
            _dumped = true;
        }
        _render_frame_count++;
    }
    (void)rr.command.End();

    VkPipelineStageFlags wait_dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo         sub_info {
                .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext                = nullptr,
                .waitSemaphoreCount   = 1,
                .pWaitSemaphores      = m_sem_image_available[slot].address(),
                .pWaitDstStageMask    = &wait_dst_stage,
                .commandBufferCount   = 1,
                .pCommandBuffers      = rr.command.address(),
                .signalSemaphoreCount = 1,
                .pSignalSemaphores    = m_sem_render_finished[slot].address(),
    };

    // Submit with this slot's fence — consumed at the start of a future
    // drawFrame call when the same slot is re-selected.
    VVK_CHECK_DEVICE_LOST(m_device->present_queue().handle.Submit(sub_info, *rr.fence_frame));
    VkPresentInfoKHR present_info {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext              = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = m_sem_render_finished[slot].address(),
        .swapchainCount     = 1,
        .pSwapchains        = m_device->swapchain().handle().address(),
        .pImageIndices      = &image_index,
    };
    VVK_CHECK_DEVICE_LOST(m_device->present_queue().handle.Present(present_info));

    // Screenshot/pass-dump readback needs the JUST-SUBMITTED work to have
    // completed.  No more queue WaitIdle — wait locally on this slot's
    // fence only when a dump is pending; common-case returns immediately.
    bool need_readback = false;
    {
        std::lock_guard<std::mutex> lock(m_screenshot_mutex);
        need_readback |= ! m_pending_screenshot_path.empty();
    }
    {
        std::lock_guard<std::mutex> lock(m_pass_dump_mutex);
        need_readback |= ! m_pending_pass_dump_dir.empty();
    }
    if (need_readback) {
        VVK_CHECK_DEVICE_LOST(rr.fence_frame.Wait(vk_wait_time));
        takeScreenshotIfRequested(image.handle,
                                  m_device->swapchain().format(),
                                  m_device->swapchain().extent().width,
                                  m_device->swapchain().extent().height);
        dumpPassesIfRequested();
    }

    m_frame_index++;
}
void VulkanRender::Impl::drawFrameOffscreen() {
    WEK_PROFILE_SCOPE("VulkanRender::drawFrameOffscreen");
    const size_t        slot  = m_frame_index % kFramesInFlight;
    RenderingResources& rr    = m_rendering_resources[slot];
    ImageParameters     image = m_ex_swapchain->GetInprogressImage();

    // Wait on THIS slot's previous submission (two frames back) — lets
    // frame N+1 record while frame N's GPU work runs.  At steady state
    // with GPU-fast scenes the wait is ~0; with GPU-slow scenes it's the
    // GPU/CPU time delta, still less than the full GPU time.
    VVK_CHECK_DEVICE_LOST(rr.fence_frame.Wait(vk_wait_time));
    VVK_CHECK_DEVICE_LOST(rr.fence_frame.Reset());

    m_dyn_buf->setCurrentSlot(slot);

    m_finpass->setPresent(image);

    (void)rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    m_dyn_buf->recordUpload(rr.command);

    {
        static bool _dumped             = false;
        static int  _render_frame_count = 0;
        int         prepared_count = 0, skipped_count = 0;
        // Reset per-frame counters (extern from CustomShaderPass)
        extern int                         g_exec_pass_counter;
        extern int                         g_exec_frame_counter;
        extern std::unordered_set<VkImage> g_depth_inited_frame;
        g_exec_pass_counter  = 0;
        g_exec_frame_counter = _render_frame_count;
        g_depth_inited_frame.clear();
        for (auto* p : m_passes) {
            if (p->prepared()) {
                p->execute(*m_device, rr);
                prepared_count++;
            } else {
                skipped_count++;
            }
        }
        if (! _dumped) {
            LOG_INFO("render frame (offscreen): %d passes executed, %d skipped (not prepared), %zu "
                     "total",
                     prepared_count,
                     skipped_count,
                     m_passes.size());
            _dumped = true;
        }
        _render_frame_count++;
    }

    (void)rr.command.End();

    VkSubmitInfo sub_info {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext              = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers    = rr.command.address(),
    };
    VVK_CHECK_DEVICE_LOST(m_device->graphics_queue().handle.Submit(sub_info, *rr.fence_frame));

    // Screenshot/pass-dump readback needs this slot's work to be done
    // before sampling the output image.  Same per-slot fence wait as the
    // swapchain path; normal rendering pays nothing because the request
    // strings are both empty.
    bool need_readback = false;
    {
        std::lock_guard<std::mutex> lock(m_screenshot_mutex);
        need_readback |= ! m_pending_screenshot_path.empty();
    }
    {
        std::lock_guard<std::mutex> lock(m_pass_dump_mutex);
        need_readback |= ! m_pending_pass_dump_dir.empty();
    }
    if (need_readback) {
        VVK_CHECK_DEVICE_LOST(rr.fence_frame.Wait(vk_wait_time));
        takeScreenshotIfRequested(
            image.handle, m_ex_swapchain->format(), image.extent.width, image.extent.height);
        dumpPassesIfRequested();
    }

    m_ex_swapchain->renderFrame();
    m_frame_index++;
}

void VulkanRender::Impl::setRenderTargetSize(Scene& scene, rg::RenderGraph& rg) {
    auto& ext = m_device->out_extent();
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (rt.bind.enable && rt.bind.screen) {
            rt.width  = (i32)(rt.bind.scale * ext.width);
            rt.height = (i32)(rt.bind.scale * ext.height);
        }
    }
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (rt.bind.screen || ! rt.bind.enable) continue;
        auto bind_rt = scene.renderTargets.find(rt.bind.name);
        if (rt.bind.name.empty() || bind_rt == scene.renderTargets.end()) {
            LOG_ERROR("unknonw render target bind: %s", rt.bind.name.c_str());
            continue;
        }
        rt.width  = (i32)(rt.bind.scale * bind_rt->second.width);
        rt.height = (i32)(rt.bind.scale * bind_rt->second.height);
    }
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (! item.first.empty() && (rt.width * rt.height <= 4)) {
            LOG_ERROR("wrong size for render target: %s", item.first.c_str());
        } else if (rt.has_mipmap) {
            rt.mipmap_level =
                std::max(3u,
                         static_cast<uint>(std::floor(std::log2(std::min(rt.width, rt.height))))) -
                2u;
        }
    }
    scene.shaderValueUpdater->SetScreenSize((i32)ext.width, (i32)ext.height);
}

void VulkanRender::Impl::UpdateCameraFillMode(wallpaper::Scene&   scene,
                                              wallpaper::FillMode fillmode) {
    using namespace wallpaper;
    auto width  = m_device->out_extent().width;
    auto height = m_device->out_extent().height;

    if (width == 0) return;
    double sw = scene.ortho[0], sh = scene.ortho[1];
    double fboAspect = width / (double)height, sAspect = sw / sh;
    auto&  gCam = *scene.cameras.at("global");

    // 3D perspective scenes: global camera IS perspective, no separate global_perspective
    if (gCam.IsPerspective()) {
        gCam.SetAspect(fboAspect);
        gCam.Update();

        // Update ortho overlay camera for flat image layers.
        // Keep height=1.0, adjust width to match viewport aspect ratio.
        if (scene.cameras.count("global_ortho")) {
            auto& orthoCam = *scene.cameras.at("global_ortho");
            orthoCam.SetWidth(fboAspect);
            orthoCam.SetHeight(1.0);
            orthoCam.Update();
        }
        return;
    }

    auto& gPerCam = *scene.cameras.at("global_perspective");
    // assum cam
    switch (fillmode) {
    case FillMode::STRETCH:
        gCam.SetWidth(sw);
        gCam.SetHeight(sh);
        gPerCam.SetAspect(sAspect);
        gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTFIT:
        if (fboAspect < sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTCROP:
    default:
        if (fboAspect > sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    }
    gCam.Update();
    gPerCam.Update();
    scene.UpdateLinkedCamera("global");
}

void VulkanRender::Impl::clearLastRenderGraph(Scene* scene) {
    // Ensure GPU is idle before destroying resources to prevent GPUVM faults
    if (m_device && m_device->handle()) {
        m_device->handle().WaitIdle();
    }
    for (auto& p : m_passes) {
        // Passes only reference the fixed members (staging pointers) from
        // RenderingResources; slot 0 is representative.
        p->destory(*m_device, m_rendering_resources[0]);
    }
    m_passes.clear();
    m_device->tex_cache().Clear();

    // Clear MSAA image init tracking (images are being destroyed)
    {
        extern std::unordered_set<VkImage> g_msaa_color_inited;
        g_msaa_color_inited.clear();
    }

    // Release GPU images stored in Scene before VMA allocator is destroyed.
    // These are shared_ptr<VmaImageParameters> erased to shared_ptr<void>.
    if (scene) {
        scene->depthBuffer.reset();
        scene->reflectionDepthBuffer.reset();
        scene->msaaDepthBuffer.reset();
        scene->msaaReflectionDepthBuffer.reset();
        scene->msaaColorImages.clear();
    }

    m_vertex_buf->destroy();
    m_dyn_buf->destroy();

    m_vertex_buf->allocate();
    m_dyn_buf->allocate();
}

void VulkanRender::Impl::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    if (! m_inited) return;
    m_pass_loaded = false;

    auto nodes             = rg.topologicalOrder();
    auto node_release_texs = rg.getLastReadTexs(nodes);

    m_passes.clear();
    m_passes.resize(nodes.size());

    std::transform(nodes.begin(),
                   nodes.end(),
                   node_release_texs.begin(),
                   m_passes.begin(),
                   [&rg](auto& id, auto& texs) {
                       auto* pass = rg.getPass(id);
                       assert(pass != nullptr);
                       VulkanPass* vpass = static_cast<VulkanPass*>(pass);
                       // LOG_INFO("----release tex");
                       for (auto& tex : texs) {
                           vpass->addReleaseTexs(spanone<const std::string_view> { tex->key() });
                           //    LOG_INFO("%s", tex->key().data());
                       }
                       return vpass;
                   });

    m_passes.insert(m_passes.begin(), m_prepass.get());
    m_passes.push_back(m_finpass.get());

    setRenderTargetSize(scene, rg);

    scene.depthBufferCleared = false;
    scene.clearedRTs.clear();

    // Prepare-time writes to dyn_buf (UBO defaults, fillBuf zero-init)
    // must land in every slot so that after the first slot switch we
    // still see the seeded state.  We turn broadcast on for the duration
    // of pass prepare and off before rendering begins.
    m_dyn_buf->setBroadcastMode(true);
    for (auto* p : m_passes) {
        if (! p->prepared()) {
            // Slot 0's RenderingResources is handed in (staging pointers
            // are fixed across slots; per-slot fence/cmd are not used at
            // prepare time).
            p->prepare(scene, *m_device, m_rendering_resources[0]);
        }
    }
    m_dyn_buf->setBroadcastMode(false);

    // Diagnostic: log pass prepare results
    {
        int prepared_count = 0, failed_count = 0;
        for (size_t i = 0; i < nodes.size(); i++) {
            auto* vpass = static_cast<VulkanPass*>(rg.getPass(nodes[i]));
            auto* pnode = rg.getPassNode(nodes[i]);
            if (! vpass->prepared()) {
                LOG_ERROR("pass[%zu] '%.*s' FAILED to prepare",
                          i,
                          (int)pnode->name().size(),
                          pnode->name().data());
                failed_count++;
            } else {
                prepared_count++;
            }
        }
        LOG_INFO("compileRenderGraph: %d passes prepared, %d FAILED, %zu total (+prepass+finpass)",
                 prepared_count,
                 failed_count,
                 nodes.size());
    }

    // Pass-output cache gating: a cacheable pass is safe to skip on
    // later frames ONLY if its output VkImage is not overwritten by any
    // later pass in the execution order — otherwise the cached bytes are
    // gone before the next frame reads them.  Walk m_passes once to find
    // the last writer of each output handle, then tag matching
    // CustomShaderPasses.
    {
        std::unordered_map<VkImage, CustomShaderPass*> last_writer;
        for (auto* p : m_passes) {
            auto* csp = dynamic_cast<CustomShaderPass*>(p);
            if (! csp || ! csp->prepared()) continue;
            VkImage out = csp->desc().vk_output.handle;
            if (out == VK_NULL_HANDLE) continue;
            last_writer[out] = csp;
        }
        int cacheable_total = 0;
        int cache_gated     = 0;
        for (auto* p : m_passes) {
            auto* csp = dynamic_cast<CustomShaderPass*>(p);
            if (! csp || ! csp->prepared()) continue;
            if (! csp->isCacheable()) continue;
            cacheable_total++;
            VkImage out = csp->desc().vk_output.handle;
            if (out != VK_NULL_HANDLE && last_writer[out] == csp) {
                csp->setCanCache(true);
                cache_gated++;
            }
        }
        LOG_INFO("pass cache: %d cacheable, %d last-writer → can skip on re-exec",
                 cacheable_total,
                 cache_gated);
    }

    VVK_CHECK_VOID_RE(m_upload_cmd.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    }));
    m_vertex_buf->recordUpload(m_upload_cmd);
    VVK_CHECK_VOID_RE(m_upload_cmd.End());
    {
        // Use fence instead of WaitIdle for better performance
        vvk::Fence upload_fence;
        VVK_CHECK_VOID_RE(m_device->handle().CreateFence(
            VkFenceCreateInfo {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
            },
            upload_fence));

        VkSubmitInfo sub_info {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext              = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers    = m_upload_cmd.address(),
        };
        VVK_CHECK_DEVICE_LOST(m_device->graphics_queue().handle.Submit(sub_info, *upload_fence));
        VVK_CHECK_DEVICE_LOST(upload_fence.Wait(vk_wait_time));
    }
    m_pass_loaded = true;
};
