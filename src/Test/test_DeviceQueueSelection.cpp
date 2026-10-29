#include <doctest.h>

#include "Vulkan/Device.hpp"

#include <vector>

using wallpaper::vulkan::queue_selection::FindUnifiedFamily;

TEST_SUITE_BEGIN("DeviceQueueSelection");

TEST_CASE("FindUnifiedFamily: unified GPU (RADV/NV/ANV) - family 0 in both") {
    // Most desktop GPUs expose a single graphics+present family at index 0.
    CHECK(FindUnifiedFamily({ 0 }, { 0 }) == 0u);
}

TEST_CASE("FindUnifiedFamily: graphics-front presents - preferred") {
    // When the first graphics family also presents, we use it (single queue).
    CHECK(FindUnifiedFamily({ 0, 1 }, { 0, 2 }) == 0u);
}

TEST_CASE("FindUnifiedFamily: only a later graphics family presents") {
    // Pathological GPU where graphic_indexs.front() doesn't support present
    // but a later graphics family does.  The old code compared fronts only
    // and split into 2 queues; the new code finds the overlap.
    CHECK(FindUnifiedFamily({ 0, 1, 2 }, { 1, 3 }) == 1u);
}

TEST_CASE("FindUnifiedFamily: only a later present family overlaps") {
    CHECK(FindUnifiedFamily({ 2 }, { 0, 1, 2, 3 }) == 2u);
}

TEST_CASE("FindUnifiedFamily: truly split - no overlap returns UINT32_MAX") {
    // Hypothetical compute-only graphics + present-only families.  Real
    // desktop hardware doesn't seem to expose this configuration, but the
    // function must report it cleanly so the caller can log + fall back.
    CHECK(FindUnifiedFamily({ 0 }, { 1 }) == UINT32_MAX);
    CHECK(FindUnifiedFamily({ 0, 2 }, { 1, 3 }) == UINT32_MAX);
}

TEST_CASE("FindUnifiedFamily: empty graphics list returns UINT32_MAX") {
    CHECK(FindUnifiedFamily({}, { 0 }) == UINT32_MAX);
}

TEST_CASE("FindUnifiedFamily: empty present list returns UINT32_MAX") {
    CHECK(FindUnifiedFamily({ 0 }, {}) == UINT32_MAX);
}

TEST_CASE("FindUnifiedFamily: both empty returns UINT32_MAX") {
    CHECK(FindUnifiedFamily({}, {}) == UINT32_MAX);
}

TEST_SUITE_END();

// CheckGPU asks the physical device for its queue families and extensions through the
// instance dispatch table, so a table of local functions stands in for a driver.
namespace
{
std::vector<VkQueueFamilyProperties> g_fake_families;

void VKAPI_PTR fakeQueueFamilies(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* out) {
    if (out == nullptr) {
        *count = static_cast<uint32_t>(g_fake_families.size());
        return;
    }
    for (uint32_t i = 0; i < *count && i < g_fake_families.size(); i++) out[i] = g_fake_families[i];
}

VkResult VKAPI_PTR fakeNoDeviceExtensions(VkPhysicalDevice, const char*, uint32_t* count,
                                          VkExtensionProperties*) {
    *count = 0;
    return VK_SUCCESS;
}

bool gpuAcceptedWithFamilies(std::vector<VkQueueFamilyProperties> families) {
    g_fake_families = std::move(families);
    vvk::InstanceDispatch dld {};
    dld.vkGetPhysicalDeviceQueueFamilyProperties = fakeQueueFamilies;
    dld.vkEnumerateDeviceExtensionProperties     = fakeNoDeviceExtensions;
    vvk::PhysicalDevice gpu(reinterpret_cast<VkPhysicalDevice>(0x1), dld);
    return wallpaper::vulkan::Device::CheckGPU(gpu, {}, VK_NULL_HANDLE);
}
} // namespace

TEST_SUITE("Device::CheckGPU queue families") {
    TEST_CASE("a GPU whose only queue family cannot do graphics is rejected") {
        CHECK_FALSE(gpuAcceptedWithFamilies(
            { { .queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, .queueCount = 1 } }));
    }
    TEST_CASE("a GPU with a graphics family is accepted, wherever in the list it sits") {
        const VkQueueFamilyProperties graphics { .queueFlags = VK_QUEUE_GRAPHICS_BIT,
                                                 .queueCount = 1 };
        const VkQueueFamilyProperties transfer { .queueFlags = VK_QUEUE_TRANSFER_BIT,
                                                 .queueCount = 1 };
        CHECK(gpuAcceptedWithFamilies({ graphics }));
        CHECK(gpuAcceptedWithFamilies({ transfer, graphics }));
    }
    TEST_CASE("a GPU with no queue families is rejected") {
        CHECK_FALSE(gpuAcceptedWithFamilies({}));
    }
}
