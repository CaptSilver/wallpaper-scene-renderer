#include <doctest.h>

#include "Image.hpp"
#include "ImagePayload.h"

#include <cstdint>

using namespace wallpaper;

namespace
{
// Adds one slot with `mipCount` mips, each `bytes` long, filled with 0xAB.
// Each mip's deleter increments *freedCounter so a test can prove the
// payload's custom deleter ran exactly once on release (no leak/double-free).
void addSlot(Image& img, int mipCount, int bytes, int* freedCounter) {
    Image::Slot slot;
    slot.width  = 64;
    slot.height = 64;
    for (int m = 0; m < mipCount; ++m) {
        ImageData mip;
        mip.width  = 64 >> m;
        mip.height = 64 >> m;
        mip.size   = bytes;
        auto* raw  = new uint8_t[(size_t)bytes];
        for (int i = 0; i < bytes; ++i) raw[i] = 0xAB;
        mip.data = ImageDataPtr(raw, [freedCounter](uint8_t* p) {
            if (freedCounter) (*freedCounter)++;
            delete[] p;
        });
        slot.mipmaps.push_back(std::move(mip));
    }
    img.slots.push_back(std::move(slot));
}
} // namespace

TEST_SUITE("Image decoded-payload release") {
    TEST_CASE("release frees every mip's bytes and zeroes its size") {
        Image img;
        int   freed = 0;
        addSlot(img, /*mips*/ 3, /*bytes*/ 256, &freed);
        addSlot(img, /*mips*/ 1, /*bytes*/ 512, &freed);

        releaseDecodedPayload(img);

        int totalMips = 0;
        for (auto& slot : img.slots)
            for (auto& mip : slot.mipmaps) {
                CHECK(mip.data.get() == nullptr);
                CHECK(mip.size == 0);
                ++totalMips;
            }
        CHECK(totalMips == 4);
        CHECK(freed == 4); // each mip's deleter ran exactly once
    }

    TEST_CASE("release preserves the shell (header, key, slot/mip dims)") {
        Image img;
        img.key                   = "_text_42";
        img.header.format         = TextureFormat::RGBA8;
        img.header.width          = 64;
        img.header.height         = 32;
        img.header.isVideoTexture = false;
        img.header.videoFilePath  = "/tmp/video_tex/foo.mp4";
        addSlot(img, /*mips*/ 2, /*bytes*/ 128, nullptr);
        const i32 slot0w = img.slots[0].width;
        const i32 mip1w  = img.slots[0].mipmaps[1].width;

        releaseDecodedPayload(img);

        CHECK(img.key == "_text_42");
        CHECK(img.header.format == TextureFormat::RGBA8);
        CHECK(img.header.width == 64);
        CHECK(img.header.height == 32);
        CHECK(img.header.videoFilePath == "/tmp/video_tex/foo.mp4");
        REQUIRE(img.slots.size() == 1u);
        REQUIRE(img.slots[0].mipmaps.size() == 2u); // mip entries kept, only .data freed
        CHECK(img.slots[0].width == slot0w);
        CHECK(img.slots[0].mipmaps[1].width == mip1w);
    }

    TEST_CASE("policy: static textures are releasable, video textures are not") {
        Image staticImg;
        staticImg.header.isVideoTexture = false;
        CHECK(mayReleaseDecodedPayload(staticImg) == true);

        Image videoImg;
        videoImg.header.isVideoTexture = true;
        CHECK(mayReleaseDecodedPayload(videoImg) == false);
    }

    TEST_CASE("policy-gated combo leaves a video placeholder intact") {
        // Mirrors the production call: `if (mayRelease) release(...)`.
        //
        // `freed` must outlive `videoImg`: the slot's mip deleter captures
        // `&freed` and runs from `Image::~Image()` when this scope unwinds.
        // Stack destruction is LIFO, so `freed` is declared FIRST to ensure
        // it is still live when the deleter fires.  Reversing the order
        // produces a stack-use-after-scope (caught by ASAN).
        int   freed = 0;
        Image videoImg;
        videoImg.header.isVideoTexture = true;
        addSlot(videoImg, /*mips*/ 1, /*bytes*/ 64, &freed);

        if (mayReleaseDecodedPayload(videoImg)) releaseDecodedPayload(videoImg);

        CHECK(videoImg.slots[0].mipmaps[0].data.get() != nullptr); // not freed
        CHECK(videoImg.slots[0].mipmaps[0].size == 64);
        CHECK(freed == 0);
    }

    TEST_CASE("release is idempotent (second call is a no-op)") {
        Image img;
        int   freed = 0;
        addSlot(img, /*mips*/ 2, /*bytes*/ 32, &freed);
        releaseDecodedPayload(img);
        releaseDecodedPayload(img); // must not double-free
        CHECK(freed == 2);
        for (auto& slot : img.slots)
            for (auto& mip : slot.mipmaps) {
                CHECK(mip.data.get() == nullptr);
                CHECK(mip.size == 0);
            }
    }

    TEST_CASE("release on an empty image is safe") {
        Image img; // no slots
        releaseDecodedPayload(img);
        CHECK(img.slots.empty());
    }
} // TEST_SUITE "Image decoded-payload release"
