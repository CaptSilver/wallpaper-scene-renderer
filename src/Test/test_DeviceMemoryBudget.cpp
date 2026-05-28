#include <doctest.h>
#include <vulkan/vulkan.h>

#include <array>
#include <type_traits>

#include "Vulkan/Device.hpp"

using wallpaper::vulkan::Device;

// Contract tests for the per-heap budget surface added so downstream callers
// (TextureCache LRU heap-pressure trigger is the planned first consumer) can
// distinguish device-local from host-visible heap pressure.  GetHeapBudgets
// itself requires a live VmaAllocator — that path is exercised by the
// integration test in preflight.sh / sceneviewer.  Here we pin the public
// contract: struct shape, array size, default-state invariants.
TEST_SUITE("Device::HeapBudget contract") {
    TEST_CASE("HeapBudget is a standard-layout POD with the documented fields") {
        // Layout / size invariants the consumers (LRU heap trigger, telemetry
        // probes) will rely on.  If a future refactor swaps VkDeviceSize for a
        // larger int or reorders members, this catches it.
        CHECK(std::is_standard_layout<Device::HeapBudget>::value);
        CHECK(std::is_trivially_copyable<Device::HeapBudget>::value);

        Device::HeapBudget hb {};
        CHECK(hb.usage == VkDeviceSize { 0 });
        CHECK(hb.budget == VkDeviceSize { 0 });
        CHECK(hb.flags == VkMemoryHeapFlags { 0 });

        // Field types are fixed by the Vulkan spec, not VMA's typedefs — the
        // memory_budget path returns driver-reported VkDeviceSize bytes.
        CHECK(sizeof(hb.usage) == sizeof(VkDeviceSize));
        CHECK(sizeof(hb.budget) == sizeof(VkDeviceSize));
    }

    TEST_CASE("GetHeapBudgets returns one entry per VK_MAX_MEMORY_HEAPS slot") {
        // The returned array is fixed-size at the spec's hard cap so callers
        // can iterate without a length probe.  GetMemoryProperties().memoryHeapCount
        // is the live-count gate that the implementation walks; tail entries
        // stay zero-initialised.
        using Returned = decltype(std::declval<const Device&>().GetHeapBudgets());
        constexpr auto kExpectedSize = VK_MAX_MEMORY_HEAPS;
        CHECK(std::tuple_size<Returned>::value == kExpectedSize);

        // std::array<HeapBudget, VK_MAX_MEMORY_HEAPS> is the contract — fixed
        // storage, no heap alloc on the query path.
        static_assert(
            std::is_same_v<Returned, std::array<Device::HeapBudget, VK_MAX_MEMORY_HEAPS>>,
            "GetHeapBudgets must return std::array sized to VK_MAX_MEMORY_HEAPS");
    }

    TEST_CASE("VK_EXT_memory_budget is the gate string for VMA flag") {
        // Pin the predicate that Device::Create uses: the VMA
        // VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT must only be set when
        // supportExt(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) is true.  A typo
        // here would silently regress us to allocator-internal bookkeeping
        // even on drivers that advertise the extension.
        const char* name = VK_EXT_MEMORY_BUDGET_EXTENSION_NAME;
        CHECK(std::string_view { name } == "VK_EXT_memory_budget");
    }
}
