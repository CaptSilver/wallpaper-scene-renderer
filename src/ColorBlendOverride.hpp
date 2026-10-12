#pragma once
#include "Type.hpp"

namespace wallpaper
{

// The blend each step of an image layer uses once its colorBlendMode has a
// hardware equivalent (`override`, or Disable for none).
struct LayerBlend {
    BlendMode base;  // the layer's own image draw
    BlendMode final; // the step that composites the layer onto the scene
};

// colorBlendMode says how the finished layer composites onto the scene. The
// hardware Screen blend (Opaque) ignores alpha, so on a layer with effects the
// image draw that fills the chain's ping-pong buffer must not leave colour
// where the image is transparent: forcing Opaque there too copied that colour
// straight in, and a soft glow came out as a solid square. That draw gets
// alpha-weighted colour instead, and keeps the buffer's alpha where Opaque
// left it, so effects that read alpha see what they always saw. Additive
// already weights colour by alpha (SRC_ALPHA) and applies to both steps.
inline LayerBlend SplitColorBlendOverride(BlendMode material, BlendMode override, bool hasEffect) {
    const BlendMode final = override != BlendMode::Disable ? override : material;
    if (hasEffect && override == BlendMode::Opaque)
        return { BlendMode::PremultipliedColorOnly, final };
    return { final, final };
}

} // namespace wallpaper
