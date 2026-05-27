#include "VulkanRender/FenceWaitRetry.hpp"

#include "Utils/Logging.h"

namespace wallpaper::vulkan
{

VkResult waitFenceWithRetry(const std::function<VkResult(uint64_t)>& wait_fn, uint64_t wait_ns,
                            int max_retries) {
    for (int i = 0; i < max_retries; ++i) {
        VkResult r = wait_fn(wait_ns);
        if (r != VK_TIMEOUT) return r;
        LOG_INFO("vk fence wait timed out (%d/%d) - retrying", i + 1, max_retries);
    }
    LOG_ERROR("vk fence wait exhausted retries (%d * %llu ns); treating as device-lost",
              max_retries,
              (unsigned long long)wait_ns);
    return VK_ERROR_DEVICE_LOST;
}

} // namespace wallpaper::vulkan
