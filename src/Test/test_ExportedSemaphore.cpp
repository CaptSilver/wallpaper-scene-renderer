#include <doctest.h>

#include "Vulkan/VulkanExSwapchain.hpp"

// CreateExportedSemaphore runs through the device dispatch table, so a table of
// local functions drives it without a Vulkan device (as test_VvkCreateWrappers
// does): each failure the driver can report has to come back as false.

namespace
{

VkResult                           g_create_result { VK_SUCCESS };
VkResult                           g_get_fd_result { VK_SUCCESS };
int                                g_get_fd_calls { 0 };
VkExternalSemaphoreHandleTypeFlags g_requested_export { 0 };

VkResult VKAPI_PTR fakeCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*     ci,
                                       const VkAllocationCallbacks*, VkSemaphore* out) {
    g_requested_export = 0;
    for (auto* n = static_cast<const VkBaseInStructure*>(ci->pNext); n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO)
            g_requested_export =
                reinterpret_cast<const VkExportSemaphoreCreateInfo*>(n)->handleTypes;
    }
    *out = reinterpret_cast<VkSemaphore>(0x5);
    return g_create_result;
}
VkResult VKAPI_PTR fakeGetSemaphoreFd(VkDevice, const VkSemaphoreGetFdInfoKHR*, int* fd) {
    ++g_get_fd_calls;
    *fd = 42;
    return g_get_fd_result;
}
void VKAPI_PTR fakeDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
void VKAPI_PTR fakeDestroyDevice(VkDevice, const VkAllocationCallbacks*) {}

vvk::DeviceDispatch makeDispatch() {
    vvk::DeviceDispatch dld {};
    dld.vkCreateSemaphore   = fakeCreateSemaphore;
    dld.vkGetSemaphoreFdKHR = fakeGetSemaphoreFd;
    dld.vkDestroySemaphore  = fakeDestroySemaphore;
    dld.vkDestroyDevice     = fakeDestroyDevice;
    return dld;
}

void reset(VkResult create, VkResult get_fd) {
    g_create_result    = create;
    g_get_fd_result    = get_fd;
    g_get_fd_calls     = 0;
    g_requested_export = 0;
}

} // namespace

TEST_SUITE("CreateExportedSemaphore") {
    using wallpaper::vulkan::CreateExportedSemaphore;

    TEST_CASE("a semaphore created for opaque-fd export hands back its fd") {
        reset(VK_SUCCESS, VK_SUCCESS);
        auto           dld = makeDispatch();
        vvk::Device    dev(reinterpret_cast<VkDevice>(0x1), dld);
        vvk::Semaphore sem;
        int            fd = -1;
        CHECK(CreateExportedSemaphore(dev, sem, fd));
        CHECK(fd == 42);
        CHECK(g_requested_export == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
    }

    TEST_CASE("a semaphore the driver will not create fails without asking for an fd") {
        reset(VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_SUCCESS);
        auto           dld = makeDispatch();
        vvk::Device    dev(reinterpret_cast<VkDevice>(0x1), dld);
        vvk::Semaphore sem;
        int            fd = -1;
        CHECK_FALSE(CreateExportedSemaphore(dev, sem, fd));
        CHECK(g_get_fd_calls == 0);
    }

    TEST_CASE("a semaphore the driver will not export fails") {
        reset(VK_SUCCESS, VK_ERROR_TOO_MANY_OBJECTS);
        auto           dld = makeDispatch();
        vvk::Device    dev(reinterpret_cast<VkDevice>(0x1), dld);
        vvk::Semaphore sem;
        int            fd = -1;
        CHECK_FALSE(CreateExportedSemaphore(dev, sem, fd));
        CHECK(g_get_fd_calls == 1);
    }
}
