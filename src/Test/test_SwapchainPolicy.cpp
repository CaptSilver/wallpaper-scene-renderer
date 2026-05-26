#include <doctest.h>
#include "Vulkan/SwapchainPolicy.hpp"

using namespace wallpaper::vulkan;

TEST_SUITE("SwapchainPolicy::classifySwapResult") {
    TEST_CASE("VK_SUCCESS is Ok") {
        CHECK(classifySwapResult(VK_SUCCESS) == SwapResult::Ok);
    }
    TEST_CASE("VK_SUBOPTIMAL_KHR is Ok (swapchain still usable)") {
        CHECK(classifySwapResult(VK_SUBOPTIMAL_KHR) == SwapResult::Ok);
    }
    TEST_CASE("VK_ERROR_OUT_OF_DATE_KHR signals recreate (Wayland resize)") {
        CHECK(classifySwapResult(VK_ERROR_OUT_OF_DATE_KHR) == SwapResult::NeedsRecreate);
    }
    TEST_CASE("VK_ERROR_SURFACE_LOST_KHR signals recreate") {
        CHECK(classifySwapResult(VK_ERROR_SURFACE_LOST_KHR) == SwapResult::NeedsRecreate);
    }
    TEST_CASE("VK_ERROR_DEVICE_LOST is fatal") {
        CHECK(classifySwapResult(VK_ERROR_DEVICE_LOST) == SwapResult::Fatal);
    }
    TEST_CASE("VK_ERROR_OUT_OF_HOST_MEMORY is fatal") {
        CHECK(classifySwapResult(VK_ERROR_OUT_OF_HOST_MEMORY) == SwapResult::Fatal);
    }
}
