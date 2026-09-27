#pragma once

// Decides which image lands in which "Now Playing" system-texture slot on a
// track change, and does it synchronously on the GUI thread at event-arrival
// time -- not at render-thread drain time. The render thread's job is purely
// mechanical (reupload whatever this handed it); if the shuffle decision
// lived there instead, two track changes landing in the same frame window
// (rapid playerctl next, or fast MPRIS metadata churn) would only ever see
// the SECOND one, so "Previous" would get promoted from whatever was drawn
// several tracks ago rather than the track immediately before it. Every
// GUI-thread event is handled in order before the next one can arrive, so
// running the decision here doesn't have that gap.
//
// No Qt, no Vulkan -- SceneObject owns an instance of this and translates
// its resolved uploads into IPropertyDispatchSink::updateSystemTexture calls.
//
// Method bodies live in the .cpp, not here: this header is reached from
// SceneBackend.hpp, which pulls in Qt headers that #define `slots` away --
// wallpaper::Image has a member literally named `slots`, so any inline body
// that touches it would silently stop parsing in that translation unit (see
// memory/qt-slots-keyword-breaks-scene-headers.md). A forward-declared Image
// is enough for the shared_ptr<Image> members below.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Scene/include/Scene/Scene.h" // MediaTextureSlot (by value in apply())

namespace wallpaper
{
struct Image;
}

namespace scenebackend
{

struct SystemTextureUpload {
    std::string                       key;
    std::shared_ptr<wallpaper::Image> image;
};

class MediaThumbnailCrossfade {
public:
    // `currentSlot` / `previousSlot` are the registry entries for
    // $mediaThumbnail / $mediaPreviousThumbnail respectively (whichever
    // exist in this scene); `newImage` is the freshly resized album art, or
    // null for "no art on this track" / a decode failure.
    std::vector<SystemTextureUpload> apply(std::shared_ptr<wallpaper::Image>          newImage,
                                           std::optional<wallpaper::MediaTextureSlot> currentSlot,
                                           std::optional<wallpaper::MediaTextureSlot> previousSlot);

    // Scene reload: a cover from the previous wallpaper must not be promoted
    // into the new one's Previous slot.
    void reset();

private:
    std::shared_ptr<wallpaper::Image> m_current;
};

} // namespace scenebackend
