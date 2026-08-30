// Unit tests for the swapchain present-mode picker.  These exercise the
// PRODUCTION function in Vulkan/SwapchainPolicy.hpp — an earlier version of
// this file carried a hand-maintained clone of the body, which would have gone
// on passing even if the real picker were deleted outright.

#include <doctest.h>
#include <vulkan/vulkan.h>
#include <vector>

#include "Vulkan/SwapchainPolicy.hpp"

using wallpaper::vulkan::PresentModePolicy;
using wallpaper::vulkan::pickPresentMode;

TEST_SUITE("pickPresentMode")
{

TEST_CASE("Auto with Fps > refresh prefers MAILBOX when available") {
    std::vector<VkPresentModeKHR> supported = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    CHECK(pickPresentMode(supported, PresentModePolicy::Auto, 120, 60)
          == VK_PRESENT_MODE_MAILBOX_KHR);
}

TEST_CASE("Auto with Fps < refresh prefers FIFO_RELAXED") {
    std::vector<VkPresentModeKHR> supported = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
    };
    CHECK(pickPresentMode(supported, PresentModePolicy::Auto, 15, 60)
          == VK_PRESENT_MODE_FIFO_RELAXED_KHR);
}

TEST_CASE("Auto with matched Fps stays on FIFO") {
    std::vector<VkPresentModeKHR> supported = { VK_PRESENT_MODE_FIFO_KHR };
    CHECK(pickPresentMode(supported, PresentModePolicy::Auto, 60, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
}

TEST_CASE("Auto within +/-10% slack stays on FIFO") {
    std::vector<VkPresentModeKHR> supported = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
    };
    // 60 fps on a 59Hz output (e.g. 59.94Hz NTSC) — within slack, stay FIFO.
    CHECK(pickPresentMode(supported, PresentModePolicy::Auto, 60, 59)
          == VK_PRESENT_MODE_FIFO_KHR);
    // 55 fps on a 60Hz output: 60 * 9 / 10 = 54.  55 > 54 → still FIFO.
    CHECK(pickPresentMode(supported, PresentModePolicy::Auto, 55, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
}

TEST_CASE("missing preferred mode falls back to FIFO") {
    std::vector<VkPresentModeKHR> supported = { VK_PRESENT_MODE_FIFO_KHR };
    CHECK(pickPresentMode(supported, PresentModePolicy::Mailbox, 120, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
    CHECK(pickPresentMode(supported, PresentModePolicy::FifoRelaxed, 15, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
    CHECK(pickPresentMode(supported, PresentModePolicy::Immediate, 60, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
    // Auto with MAILBOX unsupported but Fps > refresh: should still fall
    // through to FIFO (not crash, not pick something else from the list).
    CHECK(pickPresentMode(supported, PresentModePolicy::Auto, 240, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
}

TEST_CASE("explicit policies override Auto thresholds") {
    std::vector<VkPresentModeKHR> all = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    // Matched Fps but user explicitly asked for MAILBOX.
    CHECK(pickPresentMode(all, PresentModePolicy::Mailbox, 30, 60)
          == VK_PRESENT_MODE_MAILBOX_KHR);
    // Matched Fps but user pinned FIFO.
    CHECK(pickPresentMode(all, PresentModePolicy::Fifo, 30, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
    // Matched Fps but user pinned IMMEDIATE.
    CHECK(pickPresentMode(all, PresentModePolicy::Immediate, 30, 60)
          == VK_PRESENT_MODE_IMMEDIATE_KHR);
    // High Fps but user pinned FIFO_RELAXED.
    CHECK(pickPresentMode(all, PresentModePolicy::FifoRelaxed, 120, 60)
          == VK_PRESENT_MODE_FIFO_RELAXED_KHR);
}

} // TEST_SUITE
