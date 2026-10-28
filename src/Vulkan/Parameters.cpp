#include "Parameters.hpp"
#include <unistd.h>

namespace wallpaper
{
namespace vulkan
{

VmaBufferParameters::VmaBufferParameters()  = default;
VmaBufferParameters::~VmaBufferParameters() = default;
VmaBufferParameters::VmaBufferParameters(VmaBufferParameters&& o) noexcept
    : handle(std::move(o.handle)), req_size(o.req_size) {};
VmaBufferParameters& VmaBufferParameters::operator=(VmaBufferParameters&& o) noexcept {
    handle   = std::move(o.handle);
    req_size = o.req_size;
    return *this;
};

VmaImageParameters::VmaImageParameters()  = default;
VmaImageParameters::~VmaImageParameters() = default;
VmaImageParameters::VmaImageParameters(VmaImageParameters&& o) noexcept
    : handle(std::move(o.handle)),
      view(std::move(o.view)),
      mip0_view(std::move(o.mip0_view)),
      // sampler is a non-owning raw VkSampler (owned by TextureCache's
      // m_sampler_cache); plain value copy.
      sampler(o.sampler),
      extent(o.extent),
      mipmap_level(o.mipmap_level),
      initial_layout_transitioned(o.initial_layout_transitioned),
      dedicated_token(std::move(o.dedicated_token)) {}
VmaImageParameters& VmaImageParameters::operator=(VmaImageParameters&& o) noexcept {
    handle                      = std::move(o.handle);
    view                        = std::move(o.view);
    mip0_view                   = std::move(o.mip0_view);
    sampler                     = o.sampler; // non-owning raw VkSampler
    extent                      = o.extent;
    mipmap_level                = o.mipmap_level;
    initial_layout_transitioned = o.initial_layout_transitioned;
    dedicated_token             = std::move(o.dedicated_token);
    return *this;
}

ExImageParameters::ExImageParameters() = default;
ExImageParameters::~ExImageParameters() {
    if (fd >= 0) close(fd);
}
ExImageParameters::ExImageParameters(ExImageParameters&& o) noexcept
    : mem(std::move(o.mem)),
      mem_reqs(o.mem_reqs),
      handle(std::move(o.handle)),
      view(std::move(o.view)),
      // sampler is a non-owning raw VkSampler (owned by TextureCache's
      // m_sampler_cache); plain value copy.
      sampler(o.sampler),
      extent(o.extent),
      mipmap_level(o.mipmap_level),
      fd(std::exchange(o.fd, -1)) {}
ExImageParameters& ExImageParameters::operator=(ExImageParameters&& o) noexcept {
    mem          = std::move(o.mem);
    mem_reqs     = o.mem_reqs;
    handle       = std::move(o.handle);
    view         = std::move(o.view);
    sampler      = o.sampler; // non-owning raw VkSampler
    extent       = o.extent;
    mipmap_level = o.mipmap_level;
    if (fd >= 0 && fd != o.fd) close(fd);
    fd = std::exchange(o.fd, -1);
    return *this;
}

ImageSlots::ImageSlots()  = default;
ImageSlots::~ImageSlots() = default;
ImageSlots::ImageSlots(ImageSlots&& o) noexcept: slots(std::move(o.slots)) {}
ImageSlots& ImageSlots::operator=(ImageSlots&& o) noexcept {
    slots = std::move(o.slots);
    return *this;
}

ImageSlotsRef::ImageSlotsRef()  = default;
ImageSlotsRef::~ImageSlotsRef() = default;
ImageSlotsRef::ImageSlotsRef(const ImageSlots& o)
    : slots(std::vector<ImageParameters>(o.slots.begin(), o.slots.end())) {}

} // namespace vulkan
} // namespace wallpaper
