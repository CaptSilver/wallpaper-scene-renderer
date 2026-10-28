#include <doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
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

// Guard bytes after the converted pixels: a loop bound one too far writes
// the next pixel or channel over them instead of stopping at the row's end.
TEST_SUITE("swapchain readback row bounds") {
    constexpr uint8_t kGuard = 0xAB;

    TEST_CASE("an 8-bit row writes its pixels' RGB and nothing past them") {
        const uint8_t src[] = { 1, 2, 3, 4, 11, 12, 13, 14, 21, 22, 23, 24 };
        uint8_t       rgb[9];
        std::memset(rgb, kGuard, sizeof rgb);
        REQUIRE(SwapchainRowToRGB8(src, VK_FORMAT_R8G8B8A8_UNORM, 2, rgb));
        CHECK(rgb[3] == 11);
        CHECK(rgb[5] == 13);
        for (int i = 6; i < 9; i++) {
            CAPTURE(i);
            CHECK(rgb[i] == kGuard);
        }
    }

    TEST_CASE("a half-float row writes its pixels' RGB and nothing past them") {
        std::array<uint8_t, 24> src {};
        const auto              p0 = halfPixel(kHalfOne, kHalfOne, kHalfOne);
        const auto              p1 = halfPixel(kHalfHalf, kHalfHalf, kHalfHalf);
        const auto              p2 = halfPixel(kHalfFour, kHalfFour, kHalfFour);
        std::memcpy(src.data() + 0, p0.data(), 8);
        std::memcpy(src.data() + 8, p1.data(), 8);
        std::memcpy(src.data() + 16, p2.data(), 8);
        uint8_t rgb[9];
        std::memset(rgb, kGuard, sizeof rgb);
        REQUIRE(SwapchainRowToRGB8(src.data(), VK_FORMAT_R16G16B16A16_SFLOAT, 2, rgb));
        for (int i = 6; i < 9; i++) {
            CAPTURE(i);
            CHECK(rgb[i] == kGuard);
        }
    }
}

// The pass dump (--dump-passes-dir) writes intermediate render targets, which
// on an HDR scene hold unbounded linear colour: it tonemaps them with
// FinPass's exposure curve `1 - exp(-x)` instead of the swapchain's clamp, and
// splits 8-bit alpha into its own PGM.
TEST_SUITE("pass dump readback") {
    // What the dump writes for one half-float channel, from the curve itself.
    uint8_t tonemapped(float v) {
        return static_cast<uint8_t>(
            std::clamp((1.0f - std::exp(-std::max(0.0f, v))) * 255.0f, 0.0f, 255.0f));
    }

    TEST_CASE("an 8-bit RGBA row keeps its channels and splits out alpha") {
        const uint8_t src[] = { 1, 2, 3, 4, 11, 12, 13, 14, 21, 22, 23, 24 };
        uint8_t       rgb[9] {}, alpha[3] {};
        REQUIRE(PassDumpRowToRGB8(src, VK_FORMAT_R8G8B8A8_UNORM, 3, 0, rgb, alpha));
        const uint8_t want_rgb[] = { 1, 2, 3, 11, 12, 13, 21, 22, 23 };
        const uint8_t want_a[]   = { 4, 14, 24 };
        CHECK(std::memcmp(rgb, want_rgb, sizeof rgb) == 0);
        CHECK(std::memcmp(alpha, want_a, sizeof alpha) == 0);
    }

    TEST_CASE("an 8-bit BGRA row is swizzled to RGB and alpha split out") {
        const uint8_t src[] = { 3, 2, 1, 4, 13, 12, 11, 14, 23, 22, 21, 24 };
        uint8_t       rgb[9] {}, alpha[3] {};
        REQUIRE(PassDumpRowToRGB8(src, VK_FORMAT_B8G8R8A8_SRGB, 3, 0, rgb, alpha));
        const uint8_t want_rgb[] = { 1, 2, 3, 11, 12, 13, 21, 22, 23 };
        const uint8_t want_a[]   = { 4, 14, 24 };
        CHECK(std::memcmp(rgb, want_rgb, sizeof rgb) == 0);
        CHECK(std::memcmp(alpha, want_a, sizeof alpha) == 0);
    }

    TEST_CASE("a half-float row is exposure-tonemapped, not clamped") {
        constexpr uint16_t kHalfTwoHalf = 0x4100; // 2.5
        constexpr uint16_t kHalfBig     = 0x63D0; // 1000
        REQUIRE(Half2Float(kHalfTwoHalf) == 2.5f);
        REQUIRE(Half2Float(kHalfBig) == 1000.0f);
        std::array<uint8_t, 24> src {};
        const auto              p0 = halfPixel(kHalfZero, kHalfHalf, kHalfOne);
        const auto              p1 = halfPixel(kHalfTwoHalf, kHalfNeg, kHalfFour);
        const auto              p2 = halfPixel(kHalfBig, kHalfOne, kHalfZero);
        std::memcpy(src.data() + 0, p0.data(), 8);
        std::memcpy(src.data() + 8, p1.data(), 8);
        std::memcpy(src.data() + 16, p2.data(), 8);
        uint8_t rgb[9] {}, alpha[3] { 7, 7, 7 };
        REQUIRE(PassDumpRowToRGB8(src.data(), VK_FORMAT_R16G16B16A16_SFLOAT, 3, 0, rgb, alpha));
        const uint8_t want[] = { tonemapped(0.0f),    tonemapped(0.5f),  tonemapped(1.0f),
                                 tonemapped(2.5f),    tonemapped(-1.0f), tonemapped(4.0f),
                                 tonemapped(1000.0f), tonemapped(1.0f),  tonemapped(0.0f) };
        for (int i = 0; i < 9; i++) {
            CAPTURE(i);
            CHECK(rgb[i] == want[i]);
        }
        // The curve, not a clamp: 1.0 lands well below white, a large value at it.
        CHECK(rgb[2] < 200);
        CHECK(rgb[6] == 255);
        CHECK(rgb[0] == 0);
        CHECK(rgb[4] == 0); // negative clamps to black
        // Half-float targets carry no alpha the dump writes.
        CHECK(alpha[0] == 7);
        CHECK(alpha[2] == 7);
    }

    TEST_CASE("row y is read at y rows of the image's width in") {
        // Two rows of two RGBA8 pixels; row 1 must not be read from row 0.
        const uint8_t image[] = { 1, 2, 3, 4, 5, 6, 7, 8, 11, 12, 13, 14, 15, 16, 17, 18 };
        uint8_t       rgb[6] {}, alpha[2] {};
        REQUIRE(PassDumpRowToRGB8(image, VK_FORMAT_R8G8B8A8_UNORM, 2, 1, rgb, alpha));
        const uint8_t want_rgb[] = { 11, 12, 13, 15, 16, 17 };
        const uint8_t want_a[]   = { 14, 18 };
        CHECK(std::memcmp(rgb, want_rgb, sizeof rgb) == 0);
        CHECK(std::memcmp(alpha, want_a, sizeof alpha) == 0);
    }

    TEST_CASE("half-float row y is read at y rows of eight-byte pixels in") {
        std::array<uint8_t, 32> image {};
        const auto              row1a = halfPixel(kHalfOne, kHalfZero, kHalfZero);
        const auto              row1b = halfPixel(kHalfZero, kHalfZero, kHalfOne);
        std::memcpy(image.data() + 16, row1a.data(), 8);
        std::memcpy(image.data() + 24, row1b.data(), 8);
        uint8_t rgb[6] {}, alpha[2] {};
        REQUIRE(PassDumpRowToRGB8(image.data(), VK_FORMAT_R16G16B16A16_SFLOAT, 2, 1, rgb, alpha));
        CHECK(rgb[0] == tonemapped(1.0f));
        CHECK(rgb[1] == 0);
        CHECK(rgb[5] == tonemapped(1.0f));
    }

    TEST_CASE("a half-float row writes its pixels' RGB and nothing past them") {
        // Two pixels converted; a third sits in the source so that reading one
        // pixel or one channel too many writes something other than the guard.
        std::array<uint8_t, 24> image {};
        const auto              p0 = halfPixel(kHalfOne, kHalfOne, kHalfOne);
        const auto              p1 = halfPixel(kHalfHalf, kHalfHalf, kHalfHalf);
        const auto              p2 = halfPixel(kHalfFour, kHalfFour, kHalfFour);
        std::memcpy(image.data() + 0, p0.data(), 8);
        std::memcpy(image.data() + 8, p1.data(), 8);
        std::memcpy(image.data() + 16, p2.data(), 8);
        constexpr uint8_t kGuard = 0xAB;
        uint8_t           rgb[9];
        std::memset(rgb, kGuard, sizeof rgb);
        uint8_t alpha[3] { kGuard, kGuard, kGuard };
        REQUIRE(PassDumpRowToRGB8(image.data(), VK_FORMAT_R16G16B16A16_SFLOAT, 2, 0, rgb, alpha));
        CHECK(rgb[3] == tonemapped(0.5f));
        CHECK(rgb[5] == tonemapped(0.5f));
        for (int i = 6; i < 9; i++) {
            CAPTURE(i);
            CHECK(rgb[i] == kGuard);
        }
    }

    TEST_CASE("a format the dump cannot read writes nothing") {
        const uint8_t src[16] {};
        uint8_t       rgb[3] { 9, 9, 9 }, alpha[1] { 9 };
        CHECK_FALSE(PassDumpRowToRGB8(src, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 0, rgb, alpha));
        CHECK(rgb[0] == 9);
        CHECK(alpha[0] == 9);
    }
}
