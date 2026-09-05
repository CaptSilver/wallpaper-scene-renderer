#include <doctest.h>
#include <vulkan/vulkan.h>

#include <cstring>
#include <new>

#include "Vulkan/Device.hpp"

using namespace wallpaper::vulkan;

// A Swapchain that has never been through Create() still has its accessors
// read by anything that touches the device before the surface exists.  The
// members they return are plain Vk structs, so without default initializers
// they carry whatever was on the stack.  Device holds one by value and its
// own constructor is user-provided, so the sub-object is default-initialized,
// never zeroed -- these cases construct the same way, over deliberately
// dirtied storage, so a pass cannot come from a lucky stack.
TEST_SUITE("Swapchain default state") {
    TEST_CASE("a swapchain that was never created reports a zeroed extent") {
        alignas(Swapchain) unsigned char storage[sizeof(Swapchain)];
        std::memset(storage, 0xAB, sizeof(storage));

        auto* swap = new (storage) Swapchain;
        CHECK(swap->extent().width == 0u);
        CHECK(swap->extent().height == 0u);
        swap->~Swapchain();
    }

    TEST_CASE("a swapchain that was never created reports no format") {
        alignas(Swapchain) unsigned char storage[sizeof(Swapchain)];
        std::memset(storage, 0xAB, sizeof(storage));

        auto* swap = new (storage) Swapchain;
        CHECK(swap->format() == VK_FORMAT_UNDEFINED);
        swap->~Swapchain();
    }

    // FIFO is the one present mode every conformant implementation supports,
    // so it is the only safe thing to claim before a mode has been picked.
    TEST_CASE("a swapchain that was never created reports FIFO") {
        alignas(Swapchain) unsigned char storage[sizeof(Swapchain)];
        std::memset(storage, 0xAB, sizeof(storage));

        auto* swap = new (storage) Swapchain;
        CHECK(swap->presentMode() == VK_PRESENT_MODE_FIFO_KHR);
        swap->~Swapchain();
    }
}

namespace
{
VkSurfaceCapabilitiesKHR caps(VkExtent2D current, VkExtent2D min, VkExtent2D max,
                              uint32_t min_images = 2, uint32_t max_images = 0) {
    VkSurfaceCapabilitiesKHR c {};
    c.currentExtent  = current;
    c.minImageExtent = min;
    c.maxImageExtent = max;
    c.minImageCount  = min_images;
    c.maxImageCount  = max_images;
    return c;
}
} // namespace

// The size the swapchain is created at comes from the driver, not from
// whatever the caller happened to ask for.  The requested extent is only a
// fallback for surfaces that decline to name a size -- Wayland reports
// 0xFFFFFFFF there, and a surface being torn down reports 0.
TEST_SUITE("Swapchain extent selection") {
    TEST_CASE("the driver's current extent wins when it is in range") {
        auto c = caps({ 1920, 1080 }, { 1, 1 }, { 16384, 16384 });
        auto e = chooseSwapchainExtent(c, { 800, 600 });
        CHECK(e.width == 1920u);
        CHECK(e.height == 1080u);
    }

    TEST_CASE("a surface with no current extent falls back to the request") {
        auto c = caps({ 0xFFFFFFFFu, 0xFFFFFFFFu }, { 1, 1 }, { 16384, 16384 });
        auto e = chooseSwapchainExtent(c, { 800, 600 });
        CHECK(e.width == 800u);
        CHECK(e.height == 600u);
    }

    TEST_CASE("a zero current extent falls back to the request") {
        auto c = caps({ 0, 0 }, { 1, 1 }, { 16384, 16384 });
        auto e = chooseSwapchainExtent(c, { 800, 600 });
        CHECK(e.width == 800u);
        CHECK(e.height == 600u);
    }

    TEST_CASE("the fallback is clamped up to the minimum the surface accepts") {
        auto c = caps({ 0, 0 }, { 640, 480 }, { 16384, 16384 });
        auto e = chooseSwapchainExtent(c, { 16, 16 });
        CHECK(e.width == 640u);
        CHECK(e.height == 480u);
    }

    TEST_CASE("the fallback is clamped down to the maximum the surface accepts") {
        auto c = caps({ 0, 0 }, { 1, 1 }, { 1024, 768 });
        auto e = chooseSwapchainExtent(c, { 8000, 8000 });
        CHECK(e.width == 1024u);
        CHECK(e.height == 768u);
    }

    // Each axis is clamped on its own -- a portrait request against a
    // landscape-capped surface must not drag the other axis with it.
    TEST_CASE("each axis is clamped independently") {
        auto c = caps({ 0, 0 }, { 320, 240 }, { 1024, 768 });
        auto e = chooseSwapchainExtent(c, { 8000, 100 });
        CHECK(e.width == 1024u);
        CHECK(e.height == 240u);
    }

    // A current extent outside the advertised range is the shape of the
    // crash this guard was added for: the driver names a size it will then
    // refuse.  Take the request instead.
    TEST_CASE("a current extent outside the advertised range is not trusted") {
        auto c = caps({ 40000, 40000 }, { 1, 1 }, { 4096, 4096 });
        auto e = chooseSwapchainExtent(c, { 1280, 720 });
        CHECK(e.width == 1280u);
        CHECK(e.height == 720u);
    }

    // minImageExtent and maxImageExtent are inclusive bounds, so an extent
    // sitting exactly on one of them is a size the driver will accept.  Each
    // axis is checked against each bound on its own: treating any of the four
    // as exclusive throws away a good driver-named size and creates the
    // swapchain at the caller's guess instead.
    TEST_CASE("a current extent exactly at the minimum width is accepted") {
        auto c = caps({ 640, 720 }, { 640, 480 }, { 1920, 1080 });
        auto e = chooseSwapchainExtent(c, { 1280, 720 });
        CHECK(e.width == 640u);
        CHECK(e.height == 720u);
    }

    TEST_CASE("a current extent exactly at the minimum height is accepted") {
        auto c = caps({ 800, 480 }, { 640, 480 }, { 1920, 1080 });
        auto e = chooseSwapchainExtent(c, { 1280, 720 });
        CHECK(e.width == 800u);
        CHECK(e.height == 480u);
    }

    TEST_CASE("a current extent exactly at the maximum width is accepted") {
        auto c = caps({ 1920, 720 }, { 640, 480 }, { 1920, 1080 });
        auto e = chooseSwapchainExtent(c, { 1280, 600 });
        CHECK(e.width == 1920u);
        CHECK(e.height == 720u);
    }

    TEST_CASE("a current extent exactly at the maximum height is accepted") {
        auto c = caps({ 800, 1080 }, { 640, 480 }, { 1920, 1080 });
        auto e = chooseSwapchainExtent(c, { 1280, 600 });
        CHECK(e.width == 800u);
        CHECK(e.height == 1080u);
    }
}

// One over the minimum buys a third image to work on while two are in the
// driver's hands, but only where the surface allows a third.
TEST_SUITE("Swapchain image count") {
    TEST_CASE("one more than the surface minimum") {
        auto c = caps({ 1, 1 }, { 1, 1 }, { 1, 1 }, /*min_images=*/2, /*max_images=*/0);
        CHECK(chooseSwapchainImageCount(c) == 3u);
    }

    TEST_CASE("capped at the surface maximum") {
        auto c = caps({ 1, 1 }, { 1, 1 }, { 1, 1 }, /*min_images=*/2, /*max_images=*/2);
        CHECK(chooseSwapchainImageCount(c) == 2u);
    }

    // maxImageCount == 0 means "no limit", not "no images allowed".
    TEST_CASE("a zero maximum means unlimited") {
        auto c = caps({ 1, 1 }, { 1, 1 }, { 1, 1 }, /*min_images=*/8, /*max_images=*/0);
        CHECK(chooseSwapchainImageCount(c) == 9u);
    }
}
