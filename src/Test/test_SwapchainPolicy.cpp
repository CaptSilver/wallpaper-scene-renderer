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

TEST_SUITE("Vulkan present target selection") {
    TEST_CASE("owning a window surface presents through the on-screen swapchain") {
        CHECK(classifyPresentTarget(/*with_surface=*/true, /*ex_swapchain_created=*/true) ==
              PresentTarget::Surface);
        // The exported swapchain is irrelevant once we own a surface.
        CHECK(classifyPresentTarget(/*with_surface=*/true, /*ex_swapchain_created=*/false) ==
              PresentTarget::Surface);
    }
    TEST_CASE("offscreen renderer presents into its exported swapchain") {
        CHECK(classifyPresentTarget(/*with_surface=*/false, /*ex_swapchain_created=*/true) ==
              PresentTarget::ExportedOffscreen);
    }
    // The exported swapchain allocates three dma-buf-backed images, and that
    // fails on a GPU that has just reset or has run out of VRAM.  There is
    // nothing to present into then, so it must be a distinct answer: the
    // renderer aborts init and the caller retries, instead of reading a format
    // off a swapchain that was never built.
    TEST_CASE("offscreen renderer whose exported swapchain failed has no present target") {
        CHECK(classifyPresentTarget(/*with_surface=*/false, /*ex_swapchain_created=*/false) ==
              PresentTarget::None);
    }
}
