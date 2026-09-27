#include <doctest.h>

#include "MediaThumbnailCrossfade.hpp"
#include "Image.hpp"

using namespace scenebackend;
using wallpaper::Image;
using wallpaper::MediaTextureSlot;

namespace
{

std::shared_ptr<Image> makeImage(int32_t w, int32_t h) {
    auto        img = std::make_shared<Image>();
    Image::Slot slot;
    slot.width  = w;
    slot.height = h;
    img->slots.push_back(std::move(slot));
    return img;
}

MediaTextureSlot makeSlot(std::string key, int32_t w, int32_t h,
                          std::shared_ptr<Image> placeholder = nullptr) {
    return MediaTextureSlot { std::move(key), w, h, {}, std::move(placeholder) };
}

} // namespace

TEST_SUITE("MediaThumbnailCrossfade") {
    TEST_CASE("first apply with no prior current: only a current-slot upload") {
        MediaThumbnailCrossfade crossfade;
        auto                    imgA    = makeImage(64, 64);
        auto                    current = makeSlot("$mediaThumbnail", 64, 64);
        auto                    prev    = makeSlot("$mediaPreviousThumbnail", 64, 64);

        auto uploads = crossfade.apply(imgA, current, prev);
        REQUIRE(uploads.size() == 1);
        CHECK(uploads[0].key == "$mediaThumbnail");
        CHECK(uploads[0].image == imgA);
    }

    TEST_CASE("second apply with a matching-size previous slot: previous then current, in order") {
        MediaThumbnailCrossfade crossfade;
        auto                    imgA    = makeImage(64, 64);
        auto                    imgB    = makeImage(64, 64);
        auto                    current = makeSlot("$mediaThumbnail", 64, 64);
        auto                    prev    = makeSlot("$mediaPreviousThumbnail", 64, 64);

        crossfade.apply(imgA, current, prev);
        auto uploads = crossfade.apply(imgB, current, prev);

        REQUIRE(uploads.size() == 2);
        CHECK(uploads[0].key == "$mediaPreviousThumbnail");
        CHECK(uploads[0].image == imgA);
        CHECK(uploads[1].key == "$mediaThumbnail");
        CHECK(uploads[1].image == imgB);
    }

    TEST_CASE("no previous slot in this scene: never a previous-slot upload") {
        MediaThumbnailCrossfade crossfade;
        auto                    imgA    = makeImage(64, 64);
        auto                    imgC    = makeImage(64, 64);
        auto                    current = makeSlot("$mediaThumbnail", 64, 64);

        crossfade.apply(imgA, current, std::nullopt);
        auto uploads = crossfade.apply(imgC, current, std::nullopt);

        REQUIRE(uploads.size() == 1);
        CHECK(uploads[0].key == "$mediaThumbnail");
    }

    TEST_CASE("prior current's size doesn't match previousSlot's: no promotion") {
        MediaThumbnailCrossfade crossfade;
        auto                    imgA    = makeImage(64, 64);
        auto                    imgB    = makeImage(64, 64);
        auto                    current = makeSlot("$mediaThumbnail", 64, 64);
        // A scene whose two slots derive from different fallback textures.
        auto prevMismatched = makeSlot("$mediaPreviousThumbnail", 32, 32);

        crossfade.apply(imgA, current, prevMismatched);
        auto uploads = crossfade.apply(imgB, current, prevMismatched);

        REQUIRE(uploads.size() == 1);
        CHECK(uploads[0].key == "$mediaThumbnail");
    }

    TEST_CASE("no art: current slot gets the placeholder (outgoing real cover still promotes to "
              "previous); a later real thumbnail is not promoted from the artless gap") {
        MediaThumbnailCrossfade crossfade;
        auto                    imgA        = makeImage(64, 64);
        auto                    placeholder = makeImage(64, 64);
        auto                    current     = makeSlot("$mediaThumbnail", 64, 64, placeholder);
        auto                    prev        = makeSlot("$mediaPreviousThumbnail", 64, 64);

        crossfade.apply(imgA, current, prev);
        // Track A (real art) -> track B (no art): A's cover still promotes to
        // Previous (there WAS a real prior cover to show while it fades),
        // and Current resets to the placeholder.
        auto gapUploads = crossfade.apply(nullptr, current, prev);
        REQUIRE(gapUploads.size() == 2);
        CHECK(gapUploads[0].key == "$mediaPreviousThumbnail");
        CHECK(gapUploads[0].image == imgA);
        CHECK(gapUploads[1].key == "$mediaThumbnail");
        CHECK(gapUploads[1].image == placeholder);

        // Track B (no art) -> track C (real art): nothing real was showing
        // during the gap, so nothing gets promoted into Previous this time.
        auto imgD         = makeImage(64, 64);
        auto laterUploads = crossfade.apply(imgD, current, prev);
        REQUIRE(laterUploads.size() == 1);
        CHECK(laterUploads[0].key == "$mediaThumbnail");
    }

    TEST_CASE("reset clears m_current: the next apply behaves like the very first call") {
        MediaThumbnailCrossfade crossfade;
        auto                    imgA    = makeImage(64, 64);
        auto                    imgB    = makeImage(64, 64);
        auto                    current = makeSlot("$mediaThumbnail", 64, 64);
        auto                    prev    = makeSlot("$mediaPreviousThumbnail", 64, 64);

        crossfade.apply(imgA, current, prev);
        crossfade.reset();
        auto uploads = crossfade.apply(imgB, current, prev);

        REQUIRE(uploads.size() == 1);
        CHECK(uploads[0].key == "$mediaThumbnail");
    }
}
