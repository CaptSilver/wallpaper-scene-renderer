#pragma once
#include "Image.hpp"

namespace wallpaper
{

// True when an image's decoded CPU bytes are safe to free after the initial
// GPU upload.  Video textures are re-uploaded every frame from a freshly
// decoded frame buffer (a separate local Image built from the decoder), not
// from this image's bytes; the registered video image holds only a one-shot
// black placeholder.  We keep the conservative rule "do not free
// video-texture payloads" so the policy stays robust if a future path ever
// re-reads the placeholder — it is a single tiny mip, so retaining it is
// negligible.  Every other (static) texture is safe to release.
inline bool mayReleaseDecodedPayload(const Image& image) {
    if (image.header.isVideoTexture) return false;
    return true;
}

// Free the heavy per-mip decoded payload while leaving the shell intact:
// header (dims/format/sample/sprite/isVideoTexture/videoFilePath), key, and
// each slot/mip's width/height are preserved, so the parser cache entry still
// answers ParseHeader and a duplicate CreateTex early-returns correctly.
// Resets each mip's data pointer (running its custom deleter once) and zeroes
// the recorded size so a stale size can never drive a later staging copy.
inline void releaseDecodedPayload(Image& image) {
    for (auto& slot : image.slots) {
        for (auto& mip : slot.mipmaps) {
            mip.data.reset();
            mip.size = 0;
        }
    }
}

} // namespace wallpaper
