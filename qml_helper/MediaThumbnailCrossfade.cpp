#include "MediaThumbnailCrossfade.hpp"

#include "Image.hpp"

namespace scenebackend
{

using wallpaper::Image;
using wallpaper::MediaTextureSlot;

std::vector<SystemTextureUpload>
MediaThumbnailCrossfade::apply(std::shared_ptr<Image>          newImage,
                               std::optional<MediaTextureSlot> currentSlot,
                               std::optional<MediaTextureSlot> previousSlot) {
    std::vector<SystemTextureUpload> uploads;

    // Promote the outgoing cover into Previous before Current is
    // overwritten -- this is what makes a wallpaper's own crossfade script
    // (e.g. a 1s flash-and-hide on the "Previous" pass) show the OLD art,
    // not a stale one from several tracks back. Only when the scene
    // actually has a Previous slot, there is a real prior cover to
    // promote, and it was registered at the same size (a mismatch means
    // this scene's two slots come from different fallback textures --
    // resizing on an ad hoc basis here would be guessing, not deciding).
    if (m_current && previousSlot && ! m_current->slots.empty() &&
        m_current->slots[0].width == previousSlot->width &&
        m_current->slots[0].height == previousSlot->height) {
        uploads.push_back({ previousSlot->textureKey, m_current });
    }

    if (currentSlot) {
        if (newImage) {
            uploads.push_back({ currentSlot->textureKey, newImage });
        } else if (currentSlot->placeholder) {
            // No art / decode failure: restore the authored "no art" bytes
            // -- a cache hit against the slot's own registered placeholder,
            // not a re-decode.
            uploads.push_back({ currentSlot->textureKey, currentSlot->placeholder });
        }
    }

    // Only a REAL cover becomes tomorrow's "previous" -- an artless gap must
    // not get promoted the next time real art shows up.
    m_current = newImage;
    return uploads;
}

void MediaThumbnailCrossfade::reset() { m_current.reset(); }

} // namespace scenebackend
