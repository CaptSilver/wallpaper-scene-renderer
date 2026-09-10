#pragma once
#include <algorithm>

#include "Core/Literals.hpp"

namespace wallpaper
{

struct TextCanvasSize {
    i32  texW;
    i32  texH;
    bool autosize_canvas;
};

// Resolve the canvas a text layer rasterizes into.
//
// Wallpapers that fill text from SceneScript at runtime commonly declare a
// stub `size` in scene.json — commonly a placeholder like [2, 2] — and rely
// on the engine to grow the canvas to fit whatever the script produces.
//
// `autosize_canvas` also decides whether the mesh is cropped to the rasterized
// glyphs instead of the authored box.
inline TextCanvasSize ResolveTextCanvasSize(float size_w, float size_h, float maxwidth,
                                            float pointsize, float dpi_scale) {
    i32       texW                 = static_cast<i32>(size_w);
    i32       texH                 = static_cast<i32>(size_h);
    const i32 placeholder_threshold = 8;

    // One line of this text, in the same units the authored box is in.  A box
    // narrower than about three of those cannot hold a useful string at the
    // point size the author chose, so it is a stub meant to grow: Game of Life
    // (3453251764) declares an 18x18 box at 8pt for a tooltip whose runtime
    // text is "Stamp", and without growth the texture clipped it to roughly
    // two characters.
    //
    // Measuring against the text's own line height rather than against
    // `maxwidth` is what keeps this narrow.  A generous maxwidth is an
    // ordinary wrap hint; treating it as a growth request also caught fully
    // authored layers.  Real-Time Earth (3557068717) sizes its clock 98x50 at
    // 12pt with maxwidth 500, and grew it while the UTC line beside it — same
    // widget, same maxwidth, wider box — stayed on the authored path, so one
    // clock rendered its two lines on two different scales.
    const float line_height    = pointsize * (96.0f / 72.0f) * 1.4f;
    const bool  box_too_narrow = (texW < static_cast<i32>(line_height * 3.0f));

    const bool autosize_canvas =
        (texW <= placeholder_threshold || texH <= placeholder_threshold || box_too_narrow);

    if (autosize_canvas) {
        // Matches WPTextRenderer's DPI multiplier — the scene ortho targets
        // Retina-scale 4K, not a 96 DPI viewer.
        i32 px = static_cast<i32>(pointsize * 96.0f / 72.0f * dpi_scale + 0.5f);
        if (px < 12) px = 12;
        // Prefer maxwidth (the author's own wrapping hint); fall back to a
        // generous 2048 otherwise.  Clamp so a pathological maxwidth cannot
        // blow up the canvas.
        i32 mw = (maxwidth > 0.0f) ? static_cast<i32>(maxwidth * dpi_scale) : 0;
        if (mw <= 0) mw = 2048;
        mw   = std::min(mw, 4096);
        texW = mw;
        // Line height at our DPI, with room for up to 8 lines of a long title.
        texH = std::max(static_cast<i32>(px * 1.4f * 8), 192);
    } else {
        // The author sized this box as if rendered at 1x DPI, but the glyphs
        // are rasterized at dpi_scale.  Scale the canvas to match or two-line
        // dynamic text clips at the bottom.
        texW = static_cast<i32>(texW * dpi_scale);
        texH = static_cast<i32>(texH * dpi_scale);
    }

    if (texW <= 0 || texH <= 0) {
        texW = 512;
        texH = 128;
    }
    return TextCanvasSize { texW, texH, autosize_canvas };
}

} // namespace wallpaper
