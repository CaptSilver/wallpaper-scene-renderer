#pragma once
#include <string>

namespace wallpaper
{

// Settle one text-layer re-rasterize attempt.
//
// A text layer's retry triggers are its dirty flags plus currentText (the
// record of what is actually on screen).  Clearing them is what tells the
// renderer "this change is done", so it may only happen once the fresh pixels
// reached the GPU.
//
// `uploaded` is the result of pushing the rasterized bitmap to the texture
// cache.  `newText` is the text that was rasterized, or nullptr when the layer
// was re-rendered with the text it already holds (a pointsize/style-only
// change).  Returns true when the change is settled, so the caller can drop the
// queued update; false means try again next frame.
template<class TextLayer>
inline bool commitTextUpload(TextLayer& layer, const std::string* newText, bool uploaded) {
    if (! uploaded) return false;
    if (newText != nullptr) layer.currentText = *newText;
    layer.pointsizeDirty = false;
    layer.textStyleDirty = false;
    return true;
}

} // namespace wallpaper
