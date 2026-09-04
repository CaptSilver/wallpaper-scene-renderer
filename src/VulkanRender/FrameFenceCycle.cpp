#include "FrameFenceCycle.hpp"

namespace wallpaper::vulkan
{

namespace
{
// The frame loop treats VK_SUBOPTIMAL_KHR as success — the image is still
// presentable — matching the caller's device-lost check.
bool frameStepOk(VkResult r) noexcept { return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR; }
} // namespace

VkResult runFrameFenceCycle(const FrameFenceCycleOps& ops, std::uint64_t& frame_index,
                            bool& submitted) {
    submitted = false;

    const VkResult waited = ops.wait_fence();
    if (! frameStepOk(waited)) return waited;

    if (! ops.record()) return VK_SUCCESS;

    // Reset only here: the submit below is the one thing that re-arms the
    // fence, and every way out above this point leaves the slot for the next
    // frame to re-pick.
    const VkResult reset = ops.reset_fence();
    if (! frameStepOk(reset)) return reset;

    const VkResult sub = ops.submit();
    if (! frameStepOk(sub)) return sub;

    frame_index++;
    submitted = true;
    return VK_SUCCESS;
}

} // namespace wallpaper::vulkan
