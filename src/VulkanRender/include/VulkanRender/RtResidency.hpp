#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>

// The key type the pass analyses use to name a render target independently of
// its VkImage: the slot id for render targets, the Vulkan handle for everything
// else.  It is a tagged struct because Vulkan leaves no free bit in a
// non-dispatchable handle to tell the two kinds apart.  Header-only so the
// doctest suite can exercise it without a Vulkan device.
namespace wallpaper::vulkan
{

// Scene-lifetime identity of a render target; see TextureCache::RtSlot.
using SlotId = std::uint32_t;

// What the pass analyses (cache safety, redraw propagation, render pass
// merging) key on.  A render target is named by its slot, so the key stays the
// same while the slot's VkImage comes and goes; a depth or MSAA image, which is
// never released, is named by its handle.  A tagged struct rather than a bit
// packed into a handle: Vulkan does not promise that non-dispatchable handles
// leave any bit free.
struct ResidencyKey {
    enum class Kind : std::uint8_t
    {
        None,
        Slot,
        Handle,
    };
    Kind          kind { Kind::None };
    std::uint64_t value { 0 };

    static ResidencyKey ForSlot(SlotId id) { return { Kind::Slot, id }; }
    // A null handle is the null key, so an unbound texture slot never matches.
    static ResidencyKey ForHandle(std::uint64_t handle) {
        return handle == 0 ? ResidencyKey {} : ResidencyKey { Kind::Handle, handle };
    }
    bool operator==(const ResidencyKey&) const = default;
};

// VkImage / VkImageView are pointers on 64-bit builds; the tests pass plain
// integers.
template<typename VkH>
ResidencyKey KeyForImage(VkH handle) {
    if constexpr (std::is_pointer_v<VkH>)
        return ResidencyKey::ForHandle(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle)));
    else
        return ResidencyKey::ForHandle(static_cast<std::uint64_t>(handle));
}

} // namespace wallpaper::vulkan

template<>
struct std::hash<wallpaper::vulkan::ResidencyKey> {
    std::size_t operator()(const wallpaper::vulkan::ResidencyKey& k) const noexcept {
        return std::hash<std::uint64_t> {}(k.value) ^
               (static_cast<std::size_t>(k.kind) << (sizeof(std::size_t) * 8 - 3));
    }
};
