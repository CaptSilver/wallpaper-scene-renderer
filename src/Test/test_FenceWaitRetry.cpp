#include <doctest.h>

#include "VulkanRender/FenceWaitRetry.hpp"
#include <vulkan/vulkan.h>

using wallpaper::vulkan::waitFenceWithRetry;

TEST_SUITE("VulkanRender fence wait retry") {
    TEST_CASE("returns SUCCESS on first try without retry") {
        int      calls = 0;
        VkResult r     = waitFenceWithRetry(
            [&](uint64_t) {
                calls++;
                return VK_SUCCESS;
            },
            1000,
            3);
        CHECK(r == VK_SUCCESS);
        CHECK(calls == 1);
    }

    TEST_CASE("retries up to max_retries on TIMEOUT") {
        int      calls = 0;
        VkResult r     = waitFenceWithRetry(
            [&](uint64_t) {
                calls++;
                return calls < 3 ? VK_TIMEOUT : VK_SUCCESS;
            },
            1000,
            5);
        CHECK(r == VK_SUCCESS);
        CHECK(calls == 3);
    }

    TEST_CASE("promotes to DEVICE_LOST after exhausting retries") {
        // Every call returns TIMEOUT; helper must give up after
        // max_retries attempts and return DEVICE_LOST so the caller's
        // VVK_CHECK_DEVICE_LOST routes through device-lost recovery.
        int      calls = 0;
        VkResult r     = waitFenceWithRetry(
            [&](uint64_t) {
                calls++;
                return VK_TIMEOUT;
            },
            1000,
            3);
        CHECK(r == VK_ERROR_DEVICE_LOST);
        CHECK(calls == 3);
    }

    TEST_CASE("passes through DEVICE_LOST from underlying wait without extra calls") {
        int      calls = 0;
        VkResult r     = waitFenceWithRetry(
            [&](uint64_t) {
                calls++;
                return VK_ERROR_DEVICE_LOST;
            },
            1000,
            3);
        CHECK(r == VK_ERROR_DEVICE_LOST);
        CHECK(calls == 1);
    }

    TEST_CASE("max_retries=0 promotes to DEVICE_LOST without a wait call") {
        // Edge: a caller that opts out of retries entirely.  No wait
        // happens; defensive corner so future callers can opt out.
        int      calls = 0;
        VkResult r     = waitFenceWithRetry(
            [&](uint64_t) {
                calls++;
                return VK_TIMEOUT;
            },
            1000,
            0);
        CHECK(r == VK_ERROR_DEVICE_LOST);
        CHECK(calls == 0);
    }

    TEST_CASE("SUBOPTIMAL_KHR returned without retry (it is a sub-success)") {
        // VVK_CHECK_DEVICE_LOST treats VK_SUBOPTIMAL_KHR as not-fatal,
        // so the helper should not consume retries on it.  The fence-wait
        // family doesn't currently return SUBOPTIMAL_KHR, but the contract
        // matches the macro's treatment.
        int      calls = 0;
        VkResult r     = waitFenceWithRetry(
            [&](uint64_t) {
                calls++;
                return VK_SUBOPTIMAL_KHR;
            },
            1000,
            3);
        CHECK(r == VK_SUBOPTIMAL_KHR);
        CHECK(calls == 1);
    }

    TEST_CASE("retry-promote-to-DEVICE_LOST is identical in result to passthrough DEVICE_LOST") {
        // The VVK_CHECK_DEVICE_LOST macro at the 4 fence-wait sites doesn't
        // distinguish "retry exhausted" from "underlying driver reported
        // DEVICE_LOST" — both result in the same VkResult.  Lock the
        // contract: both shapes produce VK_ERROR_DEVICE_LOST, no other
        // result leaks through.  Locks VK2 swapchain-recreate flag interaction:
        // both paths feed the same downstream device-lost-recovery branch.
        int      calls_a         = 0;
        int      calls_b         = 0;
        VkResult retry_exhausted = waitFenceWithRetry(
            [&](uint64_t) {
                calls_a++;
                return VK_TIMEOUT;
            },
            1000,
            3);
        VkResult passthrough = waitFenceWithRetry(
            [&](uint64_t) {
                calls_b++;
                return VK_ERROR_DEVICE_LOST;
            },
            1000,
            3);
        CHECK(retry_exhausted == VK_ERROR_DEVICE_LOST);
        CHECK(passthrough == VK_ERROR_DEVICE_LOST);
        // Behavioural difference: passthrough returns after 1 wait call;
        // retry-exhausted after max_retries calls.
        CHECK(calls_a == 3);
        CHECK(calls_b == 1);
    }
}
