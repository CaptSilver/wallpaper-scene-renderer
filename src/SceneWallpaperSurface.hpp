#pragma once
#include "SceneWallpaper.hpp"

#include <functional>
#include <string_view>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <span>
#include <cstdlib>

namespace wallpaper
{
using ReDrawCB = std::function<void()>;

struct VulkanSurfaceInfo {
    std::function<VkResult(VkInstance, VkSurfaceKHR*)> createSurfaceOp;
    std::vector<std::string>                           instanceExts;
};

struct RenderInitInfo {
    bool enable_valid_layer { false };
    bool offscreen { false };
    bool hdr_output { false };
    bool hdr_content { false };

    // Deterministic-rendering mode (engine-side only; OFF by default so normal
    // playback is byte-identical to before).  When on, the render-thread draw
    // loop feeds a FIXED per-frame dt instead of the wall-clock steady_clock
    // delta, and the thread-local particle PRNG is seeded with `rng_seed` at
    // scene load (on the render thread that steps the emitters).  Together
    // these make a headless capture reproducible run-to-run on one machine,
    // which is the prerequisite for golden-image tests (spec D11 phase-1).
    // Cross-machine FP reproducibility is explicitly out of scope (phase-2).
    bool     deterministic { false };
    double   fixed_dt { 1.0 / 60.0 };
    uint32_t rng_seed { 0x9E3779B9u };

    std::span<const std::uint8_t> uuid;
    TexTiling                     offscreen_tiling { TexTiling::OPTIMAL };
    VulkanSurfaceInfo             surface_info;

    uint16_t width { 1920 };
    uint16_t height { 1080 };
    ReDrawCB redraw_callback;
};

// Pure dt-selection used by the render-thread draw loop.  Kept as a free
// function (header-only, no Vulkan / SceneWallpaper dependency) so it can be
// unit-tested in isolation: the only nondeterminism in the scene clock comes
// from `wall_dt`, and this collapses it to a constant when the mode is on.
//   - deterministic OFF: returns the wall-clock delta unchanged (today's path).
//   - deterministic ON:  returns the fixed step, ignoring wall-clock entirely.
// The caller multiplies the result by m_speed; speed is intentionally left out
// here so the helper stays a pure function of (mode, fixed_dt, wall_dt).
inline double SelectFrameDt(bool deterministic, double fixed_dt, double wall_dt) noexcept {
    return deterministic ? fixed_dt : wall_dt;
}

// Resolve whether deterministic mode is active given the struct flag and an
// optional `WEK_DETERMINISTIC` env-var override.  The env var is a convenience
// for ad-hoc headless debugging (set WEK_DETERMINISTIC=1) that does not require
// rebuilding the viewer wiring; the struct field remains the explicit,
// testable source of truth.  Any non-empty value other than "0"/"false"
// enables it.  Returns the OR of (struct flag, env override).
inline bool ResolveDeterministic(bool struct_flag) noexcept {
    if (struct_flag) return true;
    const char* e = std::getenv("WEK_DETERMINISTIC");
    if (e == nullptr || e[0] == '\0') return false;
    std::string_view v { e };
    return ! (v == "0" || v == "false" || v == "FALSE" || v == "off" || v == "OFF");
}

} // namespace wallpaper
