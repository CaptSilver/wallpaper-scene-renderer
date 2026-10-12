#include <doctest.h>

#include <optional>

#include "Vulkan/DedicatedRtMemory.hpp"
#include "Vulkan/Parameters.hpp"

using namespace wallpaper;
using namespace wallpaper::vulkan;

TEST_SUITE("DedicatedRtMemory") {
    TEST_CASE("image size is width * height * bytes per pixel for a single level") {
        CHECK(RtImageBytes(3840, 2160, TextureFormat::RGBA8, 1) == 3840ull * 2160 * 4);
        CHECK(RtImageBytes(256, 256, TextureFormat::RGBA16F, 1) == 256ull * 256 * 8);
        CHECK(RtImageBytes(100, 50, TextureFormat::R8, 1) == 5000);
    }

    TEST_CASE("a mip chain adds each halved level, floored at 1x1") {
        // 4x4 + 2x2 + 1x1 at 4 bytes
        CHECK(RtImageBytes(4, 4, TextureFormat::RGBA8, 3) == (16 + 4 + 1) * 4);
        // more levels than the image has keep adding 1x1
        CHECK(RtImageBytes(1, 1, TextureFormat::RGBA8, 3) == 12);
    }

    TEST_CASE("zero mips counts as one and a degenerate extent is empty") {
        CHECK(RtImageBytes(8, 8, TextureFormat::R8, 0) == 64);
        CHECK(RtImageBytes(0, 8, TextureFormat::R8, 1) == 0);
        CHECK(RtImageBytes(8, -1, TextureFormat::R8, 1) == 0);
    }

    TEST_CASE("bytes per pixel per render target format") {
        CHECK(RtBytesPerPixel(TextureFormat::RGBA8) == 4);
        CHECK(RtBytesPerPixel(TextureFormat::RGBA16F) == 8);
        CHECK(RtBytesPerPixel(TextureFormat::RG16F) == 4);
        CHECK(RtBytesPerPixel(TextureFormat::R16F) == 2);
        CHECK(RtBytesPerPixel(TextureFormat::RG8) == 2);
        CHECK(RtBytesPerPixel(TextureFormat::R8) == 1);
    }

    TEST_CASE("the threshold splits the big ping-pongs from the small targets") {
        const std::uint64_t limit = 4096;
        CHECK(
            ShouldUseDedicatedMemory(RtImageBytes(3840, 2160, TextureFormat::RGBA8, 1), 0, limit));
        CHECK(ShouldUseDedicatedMemory(RtImageBytes(3840, 917, TextureFormat::RGBA8, 1), 0, limit));
        CHECK_FALSE(
            ShouldUseDedicatedMemory(RtImageBytes(256, 256, TextureFormat::RGBA8, 1), 0, limit));
        CHECK(ShouldUseDedicatedMemory(kDedicatedRtMinBytes, 0, limit));
        CHECK_FALSE(ShouldUseDedicatedMemory(kDedicatedRtMinBytes - 1, 0, limit));
    }

    TEST_CASE("the guard stops dedicating at a quarter of the device limit") {
        const std::uint64_t big = 32ull << 20;
        CHECK(ShouldUseDedicatedMemory(big, 999, 4096));
        CHECK_FALSE(ShouldUseDedicatedMemory(big, 1000, 4000));
        CHECK(ShouldUseDedicatedMemory(big, 99, 400));
        CHECK_FALSE(ShouldUseDedicatedMemory(big, 100, 400));
    }

    TEST_CASE("a huge reported limit is still capped, and a zero limit disables dedication") {
        const std::uint64_t big = 32ull << 20;
        CHECK(ShouldUseDedicatedMemory(big, kDedicatedRtMaxLive - 1, 4294967295ull));
        CHECK_FALSE(ShouldUseDedicatedMemory(big, kDedicatedRtMaxLive, 4294967295ull));
        CHECK_FALSE(ShouldUseDedicatedMemory(big, 0, 0));
    }

    TEST_CASE("the live count follows token lifetime, including moves") {
        DedicatedLiveCount c;
        CHECK(c.live() == 0);
        {
            auto a = c.take();
            auto b = c.take();
            CHECK(c.live() == 2);
            auto moved = std::move(a);
            CHECK(c.live() == 2);
            b = std::move(moved); // old b released, a's count lives on in b
            CHECK(c.live() == 1);
        }
        CHECK(c.live() == 0);
    }

    TEST_CASE("a token destroyed after its counter still balances the shared count") {
        DedicatedLiveCount::Token t;
        std::uint64_t             seen = 0;
        {
            DedicatedLiveCount c;
            t    = c.take();
            seen = c.live();
        }
        CHECK(seen == 1);
        t = DedicatedLiveCount::Token(); // drops the last reference with the counter gone
    }

    TEST_CASE("the token survives VmaImageParameters moves") {
        DedicatedLiveCount                c;
        std::optional<VmaImageParameters> slot;
        {
            VmaImageParameters img;
            img.dedicated_token = c.take();
            CHECK(c.live() == 1);
            slot.emplace(std::move(img));
        }
        CHECK(c.live() == 1);

        std::optional<VmaImageParameters> retired;
        retired = std::move(slot);
        CHECK(c.live() == 1);
        slot.reset();
        CHECK(c.live() == 1);

        // move-assign over an image that holds its own token releases that one
        VmaImageParameters other;
        other.dedicated_token = c.take();
        CHECK(c.live() == 2);
        other = std::move(*retired);
        CHECK(c.live() == 1);

        retired.reset();
        CHECK(c.live() == 1);
        other = VmaImageParameters();
        CHECK(c.live() == 0);
    }
}
