#pragma once
#include "TripleSwapchain.hpp"

#include <string_view>

namespace wallpaper
{

// The semaphore traffic of one submit that renders into an exported
// ex-swapchain image.  See TripleSwapchain for what each semaphore orders.
struct ExSubmitSync {
    bool wait_release { false };
    bool wait_render_done { false };
    bool signal_render_done { false };
};

inline ExSubmitSync PlanExSubmitSync(ExInprogressWait wait, bool interop_sync) {
    if (! interop_sync) return {};
    return ExSubmitSync {
        .wait_release       = wait == ExInprogressWait::Release,
        .wait_render_done   = wait == ExInprogressWait::DrainRenderDone,
        .signal_render_done = true,
    };
}

// Whether the ex-swapchain hands images over with semaphores.  The GL side
// decides, since it has to import them: without GL_EXT_semaphore_fd the render
// thread would wait on releases nobody can signal.  WEKDE_INTEROP_SYNC=0 turns
// it off, to tell a synchronisation fault from anything else on a given driver.
inline bool InteropSyncEnabled(bool gl_can_import_semaphores, const char* env) {
    return gl_can_import_semaphores && ! (env && std::string_view(env) == "0");
}

} // namespace wallpaper
