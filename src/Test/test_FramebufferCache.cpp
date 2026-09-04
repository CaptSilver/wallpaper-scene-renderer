#include <doctest.h>

#include <optional>

#include "VulkanRender/FramebufferCache.hpp"
#include "VulkanRender/SwapchainRecreate.hpp"

using wallpaper::vulkan::FramebufferCache;
using wallpaper::vulkan::runSwapchainRecreate;
using wallpaper::vulkan::SwapchainRecreateOps;

namespace
{

// Stand-in for the window swapchain the viewer owns.  A recreate frees every
// image view and immediately asks for the same number back, so the allocator
// handing the identical handles out again is the expected case rather than a
// corner one -- which is what turns a cache entry left over from before the
// resize into a *hit* instead of a harmless miss.
struct FakeSwapchain {
    static constexpr int kImageCount = 3;

    unsigned width { 800 };
    unsigned height { 600 };
    bool     create_succeeds { true };

    bool recreate(unsigned w, unsigned h) {
        if (! create_succeeds) return false;
        width  = w;
        height = h;
        return true;
    }

    // Recycled handle: view i keeps the same value across a recreate.
    int view(int i) const { return 100 + i; }
};

struct FakeFramebuffer {
    unsigned width;
    unsigned height;
};

} // namespace

TEST_CASE("FramebufferCache creates on miss and reuses on hit") {
    FramebufferCache<int, int> cache;
    int                        creates = 0;
    auto                       make    = [&]() -> std::optional<int> {
        return ++creates * 10;
    };

    int* a = cache.getOrCreate(1, make);
    CHECK(creates == 1);
    REQUIRE(a != nullptr);
    CHECK(*a == 10);

    int* b = cache.getOrCreate(1, make); // same key: must NOT rebuild
    CHECK(creates == 1);
    REQUIRE(b != nullptr);
    CHECK(*b == 10);
    CHECK(cache.size() == 1);

    cache.getOrCreate(2, make); // different key: rebuilds
    CHECK(creates == 2);
    CHECK(cache.size() == 2);
}

TEST_CASE("FramebufferCache clear() forces recreation") {
    FramebufferCache<int, int> cache;
    int                        creates = 0;
    auto                       make    = [&]() -> std::optional<int> {
        return ++creates;
    };

    cache.getOrCreate(7, make);
    CHECK(creates == 1);
    CHECK(cache.contains(7));

    cache.clear();
    CHECK(cache.size() == 0);
    CHECK_FALSE(cache.contains(7));

    cache.getOrCreate(7, make); // after clear: must rebuild
    CHECK(creates == 2);
}

TEST_CASE("FramebufferCache does not cache a creation that failed") {
    FramebufferCache<int, int> cache;
    int                        attempts = 0;
    auto                       make     = [&]() -> std::optional<int> {
        // First call stands for vkCreateFramebuffer returning an error.
        if (++attempts == 1) return std::nullopt;
        return 99;
    };

    CHECK(cache.getOrCreate(4, make) == nullptr);
    CHECK(cache.size() == 0);
    CHECK_FALSE(cache.contains(4));

    // A later frame has to get a real framebuffer back, not the failure
    // remembered forever.
    int* fb = cache.getOrCreate(4, make);
    REQUIRE(fb != nullptr);
    CHECK(*fb == 99);
    CHECK(cache.size() == 1);
}

TEST_CASE("swapchain recreate drops framebuffers keyed on the destroyed image views") {
    FakeSwapchain                          swap;
    FramebufferCache<int, FakeFramebuffer> cache;

    auto makeFb = [&]() -> std::optional<FakeFramebuffer> {
        return FakeFramebuffer { swap.width, swap.height };
    };

    // Steady state before the resize: one framebuffer per swapchain image view.
    for (int i = 0; i < FakeSwapchain::kImageCount; ++i) cache.getOrCreate(swap.view(i), makeFb);
    REQUIRE(cache.size() == FakeSwapchain::kImageCount);

    const bool ready = runSwapchainRecreate(SwapchainRecreateOps {
        .wait_device_idle = [] {
            return true;
        },
        .recreate =
            [&] {
                return swap.recreate(1920, 1080);
            },
        .invalidate_view_caches =
            [&] {
                cache.clear();
            },
        .replace_acquire_semaphore =
            [] {
                return true;
            },
    });
    CHECK(ready);

    // The window grew, so the recycled handle has to miss: hitting would bind a
    // framebuffer sized to the pre-resize extent, and its attachment would be a
    // view the recreate destroyed.
    FakeFramebuffer* fb = cache.getOrCreate(swap.view(0), makeFb);
    REQUIRE(fb != nullptr);
    CHECK(fb->width == 1920);
    CHECK(fb->height == 1080);
}

TEST_CASE("a swapchain recreate that fails still drops the framebuffers") {
    FakeSwapchain                          swap;
    FramebufferCache<int, FakeFramebuffer> cache;

    auto makeFb = [&]() -> std::optional<FakeFramebuffer> {
        return FakeFramebuffer { swap.width, swap.height };
    };

    for (int i = 0; i < FakeSwapchain::kImageCount; ++i) cache.getOrCreate(swap.view(i), makeFb);
    REQUIRE(cache.size() == FakeSwapchain::kImageCount);

    // Rebuilding the chain tears the old images and views down before it
    // creates their replacements, so a create that fails leaves the views just
    // as dead as one that succeeds.
    swap.create_succeeds = false;

    const bool ready = runSwapchainRecreate(SwapchainRecreateOps {
        .wait_device_idle = [] {
            return true;
        },
        .recreate =
            [&] {
                return swap.recreate(1920, 1080);
            },
        .invalidate_view_caches =
            [&] {
                cache.clear();
            },
        .replace_acquire_semaphore =
            [] {
                return true;
            },
    });

    CHECK_FALSE(ready); // caller keeps its pending flag set and retries
    CHECK(cache.size() == 0);
}

TEST_CASE("a lost device leaves the swapchain and its framebuffers alone") {
    FakeSwapchain                          swap;
    FramebufferCache<int, FakeFramebuffer> cache;
    bool                                   recreated = false, invalidated = false, resemmed = false;

    cache.getOrCreate(swap.view(0), [&]() -> std::optional<FakeFramebuffer> {
        return FakeFramebuffer { swap.width, swap.height };
    });

    // Nothing below the idle wait is safe once the device is gone: the images
    // still have work reading them as far as anyone can tell.
    const bool ready = runSwapchainRecreate(SwapchainRecreateOps {
        .wait_device_idle = [] {
            return false;
        },
        .recreate =
            [&] {
                recreated = true;
                return true;
            },
        .invalidate_view_caches =
            [&] {
                invalidated = true;
            },
        .replace_acquire_semaphore =
            [&] {
                resemmed = true;
                return true;
            },
    });

    CHECK_FALSE(ready);
    CHECK_FALSE(recreated);
    CHECK_FALSE(invalidated);
    CHECK_FALSE(resemmed);
    CHECK(cache.size() == 1);
}
