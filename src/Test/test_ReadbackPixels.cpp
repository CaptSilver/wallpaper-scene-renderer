#include <doctest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "VulkanRender/ReadbackPixels.hpp"

using namespace wallpaper::vulkan;

namespace
{
// IEEE binary16 bit patterns, so the test does not depend on a float->half encoder.
constexpr uint16_t kHalfZero = 0x0000;
constexpr uint16_t kHalfHalf = 0x3800; // 0.5
constexpr uint16_t kHalfOne  = 0x3C00; // 1.0
constexpr uint16_t kHalfFour = 0x4400; // 4.0
constexpr uint16_t kHalfNeg  = 0xBC00; // -1.0

std::array<uint8_t, 8> halfPixel(uint16_t r, uint16_t g, uint16_t b) {
    std::array<uint8_t, 8> px {};
    std::memcpy(px.data() + 0, &r, 2);
    std::memcpy(px.data() + 2, &g, 2);
    std::memcpy(px.data() + 4, &b, 2);
    std::memcpy(px.data() + 6, &kHalfOne, 2);
    return px;
}
} // namespace

TEST_SUITE("swapchain readback") {
    // The staging buffer and the row stride both come from this; an HDR
    // swapchain read as 4 bytes a pixel overran its buffer and wrote noise.
    TEST_CASE("an HDR swapchain is eight bytes a pixel") {
        CHECK(BytesPerPixelFor(VK_FORMAT_R16G16B16A16_SFLOAT) == 8);
        CHECK(BytesPerPixelFor(VK_FORMAT_B8G8R8A8_UNORM) == 4);
        CHECK(BytesPerPixelFor(VK_FORMAT_R8G8B8A8_UNORM) == 4);
        CHECK(BytesPerPixelFor(VK_FORMAT_A2B10G10R10_UNORM_PACK32) == 0);
    }

    TEST_CASE("an 8-bit BGRA row is swizzled to RGB") {
        const std::array<uint8_t, 8> src { 10, 20, 30, 255, 40, 50, 60, 255 };
        std::array<uint8_t, 6>       dst {};
        REQUIRE(SwapchainRowToRGB8(src.data(), VK_FORMAT_B8G8R8A8_UNORM, 2, dst.data()));
        CHECK(dst == std::array<uint8_t, 6> { 30, 20, 10, 60, 50, 40 });
    }

    TEST_CASE("an 8-bit RGBA row is copied as is") {
        const std::array<uint8_t, 4> src { 10, 20, 30, 255 };
        std::array<uint8_t, 3>       dst {};
        REQUIRE(SwapchainRowToRGB8(src.data(), VK_FORMAT_R8G8B8A8_UNORM, 1, dst.data()));
        CHECK(dst == std::array<uint8_t, 3> { 10, 20, 30 });
    }

    // An HDR swapchain holds the final pass's linear output; the SDR final
    // pass would clamp it and sRGB-encode it, so the capture does the same.
    TEST_CASE("a half-float row is clamped and sRGB-encoded") {
        const auto             px = halfPixel(kHalfZero, kHalfOne, kHalfFour);
        std::array<uint8_t, 3> dst {};
        REQUIRE(SwapchainRowToRGB8(px.data(), VK_FORMAT_R16G16B16A16_SFLOAT, 1, dst.data()));
        CHECK(dst[0] == 0);
        CHECK(dst[1] == 255);
        CHECK(dst[2] == 255);
    }

    TEST_CASE("half-float mid grey lands on the sRGB curve, negatives on black") {
        const auto             px = halfPixel(kHalfHalf, kHalfNeg, kHalfZero);
        std::array<uint8_t, 3> dst {};
        REQUIRE(SwapchainRowToRGB8(px.data(), VK_FORMAT_R16G16B16A16_SFLOAT, 1, dst.data()));
        CHECK(dst[0] == 188); // sRGB(0.5) = 0.7354
        CHECK(dst[1] == 0);
    }

    TEST_CASE("an unknown format converts nothing") {
        const std::array<uint8_t, 4> src {};
        std::array<uint8_t, 3>       dst {};
        CHECK_FALSE(
            SwapchainRowToRGB8(src.data(), VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, dst.data()));
    }
}
