#pragma once

#include <string_view>

namespace wallpaper
{

// Whether TRAILRENDERER=1 should be set on the genericropeparticle shader for
// a given particle renderer name.
//
// Mirrors wallpaper64.exe's combo dispatch in FUN_1401975d0 (clean-room reading
// of the renderer-kind switch on the renderer struct's first byte): only
// `spritetrail` (kind=2) and `ropetrail` (kind=4) emit `TRAILRENDERER=1`.
// Plain `sprite` (kind=1) and plain `rope` (kind=3) leave it undefined so the
// vertex shader falls into the non-trail `#else` branch.
//
// Setting TRAILRENDERER for plain rope wedges the first segment to
// `uvMinimum=0, uvDelta=0` in the vertex shader's TRAIL UV path, which samples
// the texture's V=0 edge across the whole quad — typically a transparent strip
// for beam textures.  The downstream symptom is "lightning bolts barely
// visible" on rope-renderer wallpapers like NieR 2B's Разряд / Молния.
inline bool RendererSetsTrailRendererCombo(std::string_view renderer_name) noexcept {
    return renderer_name == "spritetrail" || renderer_name == "ropetrail";
}

} // namespace wallpaper
