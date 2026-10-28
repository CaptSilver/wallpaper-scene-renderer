#pragma once

#include "Swapchain/ExSwapchain.hpp"
#include "Device.hpp"
#include <cstdio>
#include <utility>

namespace wallpaper
{
namespace vulkan
{

struct VulkanExHandle : NoCopy {
    ExHandle          handle;
    ExImageParameters image;
    // Exported to the consumer when the swapchain is synchronised, empty
    // otherwise.  See TripleSwapchain for what each one orders.
    vvk::Semaphore render_done;
    vvk::Semaphore release;
    int            render_done_fd { -1 };
    int            release_fd { -1 };

    VulkanExHandle()  = default;
    ~VulkanExHandle() = default;
    VulkanExHandle(VulkanExHandle&& o) noexcept
        : handle(o.handle),
          image(std::move(o.image)),
          render_done(std::move(o.render_done)),
          release(std::move(o.release)),
          render_done_fd(std::exchange(o.render_done_fd, -1)),
          release_fd(std::exchange(o.release_fd, -1)) {}
    VulkanExHandle& operator=(VulkanExHandle&& o) noexcept {
        handle         = o.handle;
        image          = std::move(o.image);
        render_done    = std::move(o.render_done);
        release        = std::move(o.release);
        render_done_fd = std::exchange(o.render_done_fd, -1);
        release_fd     = std::exchange(o.release_fd, -1);
        return *this;
    }
};

class VulkanExSwapchain : public ExSwapchain {
    using atomic_ = std::atomic<ExHandle*>;

public:
    VulkanExSwapchain(std::array<VulkanExHandle, 3> handles, VkExtent2D ext,
                      VkFormat fmt = kExSwapchainSdrFormat)
        : m_handles(std::move(handles)), m_extent(ext), m_format(fmt) {
        // GL_RGBA8 = 0x8058, GL_RGBA16F = 0x881A
        uint32_t gl_fmt = (fmt == kExSwapchainHdrFormat) ? 0x881Au : 0x8058u;
        int      index  = 0;
        for (auto& h : m_handles) {
            auto& handle     = h.handle;
            handle           = ExHandle(index++);
            handle.width     = (i32)h.image.extent.width;
            handle.height    = (i32)h.image.extent.height;
            handle.fd        = h.image.fd;
            handle.size      = h.image.mem_reqs.size;
            handle.gl_format = gl_fmt;
            // The consumer imports these and owns them from then on.
            handle.render_done_fd = std::exchange(h.render_done_fd, -1);
            handle.release_fd     = std::exchange(h.release_fd, -1);
        }
        m_presented  = &m_handles[0].handle;
        m_ready      = &m_handles[1].handle;
        m_inprogress = &m_handles[2].handle;
    }
    virtual ~VulkanExSwapchain() = default;

    std::uint32_t width() const override { return m_extent.width; }
    std::uint32_t height() const override { return m_extent.height; }

    const auto& handles() const { return m_handles; }

    ExImageParameters& GetInprogressImage() { return inprogressHandle().image; }

    // Whether images are handed over with semaphores.  When false the
    // semaphore accessors below return VK_NULL_HANDLE.
    bool        interopSync() const { return *m_handles[0].render_done != VK_NULL_HANDLE; }
    VkSemaphore inprogressRenderDone() { return *inprogressHandle().render_done; }
    VkSemaphore inprogressRelease() { return *inprogressHandle().release; }

    VkFormat format() const { return m_format; };

protected:
    atomic_& presented() override { return m_presented; };
    atomic_& ready() override { return m_ready; };
    atomic_& inprogress() override { return m_inprogress; };

private:
    VulkanExHandle& inprogressHandle() { return m_handles.at((usize)(*inprogress()).id()); }

    std::array<VulkanExHandle, 3> m_handles;
    atomic_                       m_presented { nullptr };
    atomic_                       m_ready { nullptr };
    atomic_                       m_inprogress { nullptr };
    VkExtent2D                    m_extent;
    VkFormat                      m_format;
};

// Build the three exportable images the host samples as dma-bufs.  Walks the
// candidate formats rather than failing on the first refusal: 16-bit float is
// only ever asked for by HDR output, and a driver that will not export it
// would otherwise leave the wallpaper blank until the setting is toggled
// back.  Ask the driver about each format before spending three allocations
// on it.  Returns nullptr only when nothing works -- the caller must fail
// init then, since there is nothing to present into.
// One binary semaphore exportable as an opaque fd, and the fd.
inline bool CreateExportedSemaphore(const Device& device, vvk::Semaphore& sem, int& fd) {
    const VkExportSemaphoreCreateInfo export_info {
        .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .pNext       = nullptr,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    const VkSemaphoreCreateInfo ci {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &export_info,
        .flags = 0,
    };
    if (device.handle().CreateSemaphore(ci, sem) != VK_SUCCESS) return false;
    return sem.GetFdKHR(&fd) == VK_SUCCESS;
}

inline std::unique_ptr<VulkanExSwapchain> CreateExSwapchain(const Device& device, uint w, uint h,
                                                            VkImageTiling tiling,
                                                            VkFormat format = kExSwapchainSdrFormat,
                                                            bool     interop_sync = false) {
    for (VkFormat candidate : exSwapchainFormatCandidates(format)) {
        if (! exSwapchainFormatUsable(device.gpu().GetFormatProperties(candidate), tiling)) {
            LOG_INFO("offscreen swapchain: driver does not support %s for this tiling",
                     vvk::ToString(candidate));
            continue;
        }
        std::array<VulkanExHandle, 3> handles;
        bool                          built = true;
        for (auto& handle : handles) {
            if (auto rv = device.tex_cache().CreateExTex(w, h, candidate, tiling); rv.has_value())
                handle.image = std::move(rv.value());
            else {
                built = false;
                break;
            }
        }
        if (! built) continue;
        // A consumer that said it can import semaphores waits on these, so
        // failing to make them is fatal rather than a quiet fallback to
        // unsynchronised handoff.
        if (interop_sync) {
            for (auto& handle : handles) {
                if (! CreateExportedSemaphore(device, handle.render_done, handle.render_done_fd) ||
                    ! CreateExportedSemaphore(device, handle.release, handle.release_fd)) {
                    LOG_ERROR("offscreen swapchain: cannot export image semaphores");
                    return nullptr;
                }
            }
        }
        if (candidate != format) {
            LOG_INFO("offscreen swapchain: %s unavailable, falling back to %s",
                     vvk::ToString(format),
                     vvk::ToString(candidate));
        }
        return std::make_unique<VulkanExSwapchain>(
            std::move(handles), VkExtent2D { w, h }, candidate);
    }
    return nullptr;
}

} // namespace vulkan
} // namespace wallpaper
