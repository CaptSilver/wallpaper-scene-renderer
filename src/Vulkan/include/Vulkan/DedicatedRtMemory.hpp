#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include "Type.hpp"

// When a managed render target gets its own VkDeviceMemory instead of a slice
// of a VMA block.  VMA hands blocks back to the driver only once every
// allocation in them is gone, so releasing a sub-allocated image frees VMA
// space but leaves the GPU footprint where it was.  A dedicated allocation goes
// back to the driver the moment the image is destroyed.  Header-only so the
// doctest suite covers the decision without a device.
namespace wallpaper
{
namespace vulkan
{

// Smallest image that gets dedicated memory.  The bytes worth returning are the
// big ping-pongs (3840x2160 RGBA8 is 32 MiB, 3840x917 about 13 MiB); hundreds
// of 256x256 targets (256 KiB) pack well into shared blocks and a driver
// allocation each would only waste address space and allocation slots.
constexpr std::uint64_t kDedicatedRtMinBytes = 4ull << 20;

// Upper bound on live dedicated allocations, whatever the driver allows.  Some
// drivers report an allocation limit near 2^32 (RADV), which says nothing about
// what a kernel is happy to track per process.
constexpr std::uint64_t kDedicatedRtMaxLive = 1024;

// A dedicated render target may take at most 1/kDedicatedLimitDivisor of the
// device's maxMemoryAllocationCount; VMA blocks, loaded textures, buffers and
// the swapchain share the rest.
constexpr std::uint64_t kDedicatedLimitDivisor = 4;

// Bytes per texel of a render target format.  Block-compressed formats are
// never render targets; they round up to one byte so a caller still gets a
// nonzero size.
inline std::uint64_t RtBytesPerPixel(TextureFormat f) {
    switch (f) {
    case TextureFormat::RGBA16F: return 8;
    case TextureFormat::RGBA8:
    case TextureFormat::RG16F:
    case TextureFormat::RGBA1010102: return 4;
    case TextureFormat::RGB8: return 3;
    case TextureFormat::RG8:
    case TextureFormat::R16F:
    case TextureFormat::RGB565: return 2;
    default: return 1;
    }
}

// Size of a 2D image with a full `mips`-level chain, from its key alone.
inline std::uint64_t RtImageBytes(std::int32_t width, std::int32_t height, TextureFormat format,
                                  std::uint32_t mips) {
    if (width <= 0 || height <= 0) return 0;
    const std::uint64_t bpp   = RtBytesPerPixel(format);
    std::uint64_t       w     = static_cast<std::uint64_t>(width);
    std::uint64_t       h     = static_cast<std::uint64_t>(height);
    std::uint64_t       total = 0;
    for (std::uint32_t level = 0; level < std::max(mips, 1u); ++level) {
        total += w * h * bpp;
        w = std::max<std::uint64_t>(1, w / 2);
        h = std::max<std::uint64_t>(1, h / 2);
    }
    return total;
}

inline bool ShouldUseDedicatedMemory(std::uint64_t image_bytes, std::uint64_t live_dedicated,
                                     std::uint64_t max_memory_allocation_count) {
    if (image_bytes < kDedicatedRtMinBytes) return false;
    const std::uint64_t budget =
        std::min(kDedicatedRtMaxLive, max_memory_allocation_count / kDedicatedLimitDivisor);
    return live_dedicated < budget;
}

// Counts live dedicated images.  Each image carries a token; the count drops
// when the token dies with the image, wherever that happens (retire ring,
// scene teardown).
class DedicatedLiveCount {
public:
    class Token {
    public:
        Token() = default;
        explicit Token(std::shared_ptr<std::atomic<std::uint64_t>> c): m_count(std::move(c)) {
            if (m_count) m_count->fetch_add(1, std::memory_order_relaxed);
        }
        Token(Token&& o) noexcept: m_count(std::move(o.m_count)) {}
        Token& operator=(Token&& o) noexcept {
            if (this != &o) {
                release();
                m_count = std::move(o.m_count);
            }
            return *this;
        }
        Token(const Token&)            = delete;
        Token& operator=(const Token&) = delete;
        ~Token() { release(); }

    private:
        void release() {
            if (m_count) m_count->fetch_sub(1, std::memory_order_relaxed);
            m_count.reset();
        }
        std::shared_ptr<std::atomic<std::uint64_t>> m_count;
    };

    std::uint64_t live() const { return m_count->load(std::memory_order_relaxed); }
    Token         take() { return Token(m_count); }

private:
    std::shared_ptr<std::atomic<std::uint64_t>> m_count {
        std::make_shared<std::atomic<std::uint64_t>>(0)
    };
};

} // namespace vulkan
} // namespace wallpaper
