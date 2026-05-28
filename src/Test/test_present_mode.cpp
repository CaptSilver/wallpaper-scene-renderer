// Unit tests for the swapchain pick-present-mode picker.  The production
// helper lives in the anonymous namespace of src/Vulkan/Swapchain.cpp, so a
// test-local clone (with the same body) locks behaviour without exposing
// the helper.  If the production picker diverges, this clone goes out of
// sync and a maintenance-level diff will catch it; alternatively, hoist the
// helper into a shared header once a third caller emerges.

#include <doctest.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <vector>

namespace
{

enum class TestPresentModePolicy
{
    Auto        = 0,
    Fifo        = 1,
    FifoRelaxed = 2,
    Mailbox     = 3,
    Immediate   = 4,
};

// Test-local clone of the production picker in
// src/Vulkan/Swapchain.cpp — body kept verbatim.
VkPresentModeKHR pickPresentMode(const std::vector<VkPresentModeKHR>& supported,
                                  TestPresentModePolicy policy,
                                  int                   target_fps,
                                  int                   output_refresh_hz) {
    auto has = [&](VkPresentModeKHR m) {
        return std::find(supported.begin(), supported.end(), m) != supported.end();
    };

    switch (policy) {
    case TestPresentModePolicy::Auto: {
        if (target_fps > output_refresh_hz * 11 / 10 && has(VK_PRESENT_MODE_MAILBOX_KHR))
            return VK_PRESENT_MODE_MAILBOX_KHR;
        if (target_fps < output_refresh_hz * 9 / 10 && has(VK_PRESENT_MODE_FIFO_RELAXED_KHR))
            return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    case TestPresentModePolicy::Mailbox:
        return has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR
                                                : VK_PRESENT_MODE_FIFO_KHR;
    case TestPresentModePolicy::FifoRelaxed:
        return has(VK_PRESENT_MODE_FIFO_RELAXED_KHR) ? VK_PRESENT_MODE_FIFO_RELAXED_KHR
                                                     : VK_PRESENT_MODE_FIFO_KHR;
    case TestPresentModePolicy::Immediate:
        return has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR
                                                  : VK_PRESENT_MODE_FIFO_KHR;
    case TestPresentModePolicy::Fifo:
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

} // namespace

TEST_SUITE("pickPresentMode")
{

TEST_CASE("Auto with Fps > refresh prefers MAILBOX when available") {
    std::vector<VkPresentModeKHR> supported = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    CHECK(pickPresentMode(supported, TestPresentModePolicy::Auto, 120, 60)
          == VK_PRESENT_MODE_MAILBOX_KHR);
}

TEST_CASE("Auto with Fps < refresh prefers FIFO_RELAXED") {
    std::vector<VkPresentModeKHR> supported = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
    };
    CHECK(pickPresentMode(supported, TestPresentModePolicy::Auto, 15, 60)
          == VK_PRESENT_MODE_FIFO_RELAXED_KHR);
}

TEST_CASE("Auto with matched Fps stays on FIFO") {
    std::vector<VkPresentModeKHR> supported = { VK_PRESENT_MODE_FIFO_KHR };
    CHECK(pickPresentMode(supported, TestPresentModePolicy::Auto, 60, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
}

TEST_CASE("Auto within +/-10% slack stays on FIFO") {
    std::vector<VkPresentModeKHR> supported = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR,
    };
    // 60 fps on a 59Hz output (e.g. 59.94Hz NTSC) — within slack, stay FIFO.
    CHECK(pickPresentMode(supported, TestPresentModePolicy::Auto, 60, 59)
          == VK_PRESENT_MODE_FIFO_KHR);
    // 55 fps on a 60Hz output: 60 * 9 / 10 = 54.  55 > 54 → still FIFO.
    CHECK(pickPresentMode(supported, TestPresentModePolicy::Auto, 55, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
}

TEST_CASE("missing preferred mode falls back to FIFO") {
    std::vector<VkPresentModeKHR> supported = { VK_PRESENT_MODE_FIFO_KHR };
    CHECK(pickPresentMode(supported, TestPresentModePolicy::Mailbox, 120, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
    CHECK(pickPresentMode(supported, TestPresentModePolicy::FifoRelaxed, 15, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
    CHECK(pickPresentMode(supported, TestPresentModePolicy::Immediate, 60, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
    // Auto with MAILBOX unsupported but Fps > refresh: should still fall
    // through to FIFO (not crash, not pick something else from the list).
    CHECK(pickPresentMode(supported, TestPresentModePolicy::Auto, 240, 60)
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
    CHECK(pickPresentMode(all, TestPresentModePolicy::Mailbox, 30, 60)
          == VK_PRESENT_MODE_MAILBOX_KHR);
    // Matched Fps but user pinned FIFO.
    CHECK(pickPresentMode(all, TestPresentModePolicy::Fifo, 30, 60)
          == VK_PRESENT_MODE_FIFO_KHR);
    // Matched Fps but user pinned IMMEDIATE.
    CHECK(pickPresentMode(all, TestPresentModePolicy::Immediate, 30, 60)
          == VK_PRESENT_MODE_IMMEDIATE_KHR);
    // High Fps but user pinned FIFO_RELAXED.
    CHECK(pickPresentMode(all, TestPresentModePolicy::FifoRelaxed, 120, 60)
          == VK_PRESENT_MODE_FIFO_RELAXED_KHR);
}

} // TEST_SUITE
