#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace wallpaper
{

struct RenderExtent {
    uint32_t width;
    uint32_t height;
};

// Smallest render target worth producing.  Below this the upscale is mush and
// the fixed per-frame costs dominate anyway, so clamping protects a user who
// drags the scale slider to its floor on a small screen.
inline constexpr uint32_t kMinRenderExtent = 64;

// Lowest render scale offered.  A quarter in each axis is a sixteenth of the
// pixels — already a drastic trade, and past it the result stops resembling
// the wallpaper.
inline constexpr double kMinRenderScale = 0.25;

// Resolve what physical pixel size to render at.
//
// item_w/item_h : the item's size in device pixels (logical size x dpr).
// scale         : the user's render scale, 1.0 being native.  Rendering below
//                 native and letting Qt upscale is the cheapest way to cut
//                 GPU cost on a heavy wallpaper, since almost everything the
//                 renderer does is per-pixel and per-pass.
// pin_w/pin_h   : an exact size pinned by the caller (the standalone viewer's
//                 -R), 0 for none.  A pin wins over the scale outright: it
//                 exists because Wayland fractional scaling makes dpr unstable
//                 around window-show time, so the caller has already decided
//                 the exact physical size it wants.
//
// Never returns zero — a zero-sized target is not a smaller render, it is a
// broken swapchain.
inline RenderExtent ResolveRenderExtent(uint32_t item_w, uint32_t item_h, double scale,
                                        uint32_t pin_w = 0, uint32_t pin_h = 0) {
    if (pin_w > 0 && pin_h > 0) return { pin_w, pin_h };

    const double s = std::clamp(scale, kMinRenderScale, 1.0);
    const auto   apply = [s](uint32_t v) {
        const auto scaled = (uint32_t)std::lround((double)v * s);
        return std::max(scaled, kMinRenderExtent);
    };
    return { apply(item_w), apply(item_h) };
}

} // namespace wallpaper
