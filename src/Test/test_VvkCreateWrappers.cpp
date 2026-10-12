#include <doctest.h>

#include <vulkan/vulkan.h>

#include <vector>

#include "vvk/vulkan_wrapper.hpp"

// The Create* wrappers run entirely through the device dispatch table, so a
// table of local functions drives them without a Vulkan device.  They own the
// handle only when the driver call succeeded.

namespace
{

VkResult                 g_create_result { VK_SUCCESS };
std::vector<VkQueryPool> g_destroyed_pools;

VkResult VKAPI_PTR fakeCreateQueryPool(VkDevice, const VkQueryPoolCreateInfo*,
                                       const VkAllocationCallbacks*, VkQueryPool* out) {
    *out = reinterpret_cast<VkQueryPool>(0x1234);
    return g_create_result;
}
void VKAPI_PTR fakeDestroyQueryPool(VkDevice, VkQueryPool pool, const VkAllocationCallbacks*) {
    g_destroyed_pools.push_back(pool);
}
void VKAPI_PTR fakeDestroyDevice(VkDevice, const VkAllocationCallbacks*) {}

vvk::DeviceDispatch makeDispatch() {
    vvk::DeviceDispatch dld {};
    dld.vkCreateQueryPool  = fakeCreateQueryPool;
    dld.vkDestroyQueryPool = fakeDestroyQueryPool;
    dld.vkDestroyDevice    = fakeDestroyDevice;
    return dld;
}

} // namespace

TEST_SUITE("vvk::Device::CreateQueryPool") {
    TEST_CASE("a successful create hands the pool to the caller, who destroys it") {
        g_create_result = VK_SUCCESS;
        g_destroyed_pools.clear();
        auto                  dld = makeDispatch();
        vvk::Device           dev(reinterpret_cast<VkDevice>(0x1), dld);
        VkQueryPoolCreateInfo ci {};
        vvk::QueryPool        pool;

        CHECK(dev.CreateQueryPool(ci, pool) == VK_SUCCESS);
        REQUIRE(static_cast<bool>(pool));
        CHECK(*pool == reinterpret_cast<VkQueryPool>(0x1234));

        pool.reset();
        REQUIRE(g_destroyed_pools.size() == 1);
        CHECK(g_destroyed_pools[0] == reinterpret_cast<VkQueryPool>(0x1234));
    }
    TEST_CASE("a failed create leaves the caller's pool empty") {
        g_create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        g_destroyed_pools.clear();
        auto                  dld = makeDispatch();
        vvk::Device           dev(reinterpret_cast<VkDevice>(0x1), dld);
        VkQueryPoolCreateInfo ci {};
        vvk::QueryPool        pool;

        CHECK(dev.CreateQueryPool(ci, pool) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
        CHECK_FALSE(static_cast<bool>(pool));
        pool.reset();
        CHECK(g_destroyed_pools.empty());
    }
}
