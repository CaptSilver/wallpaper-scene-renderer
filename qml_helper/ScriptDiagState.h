#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace scenebackend
{

// Per-loaded-wallpaper, per-SceneObject diagnostics dedup / one-shot state.
//
// These were function-local `static` containers in SceneBackend.cpp's script
// tick loops (Item 15): process-lifetime and shared across every SceneObject,
// so they were never reset on a wallpaper switch (a new scene's first error on
// a recycled node id was dropped) and were shared across monitors (screen 1's
// error suppressed screen 2's identical-id error).  Promoting them to a member
// (one ScriptDiagState per SceneObject) + clearing in cleanupTextScripts() makes
// dedup per-load AND per-instance.  Single-load logging is byte-identical; only
// the reset boundary and instance isolation change.
//
// Header-only (sets + a bool, no Qt/Vulkan deps) so scenescript_tests can lock
// the contract without SceneBackend.hpp's heavy includes — mirrors
// HoverLeaveDebounce.h / JsWatchdog.h.
//
// NOT in here (intentionally process-global — cadence limiters, not correctness
// state; resetting them would re-fire boot dumps every switch): s_gc_counter,
// s_diagTick / s_updates* / s_errorsThisWin, and the timing accumulators.
struct ScriptDiagState {
    std::unordered_set<int>         textErroredIds;   // was static @ SceneBackend.cpp:3938
    std::unordered_set<int64_t>     svErrored;        // was static @ :4146
    std::unordered_set<int>         propErroredIds;   // was static @ :4561
    std::unordered_set<std::string> dirtyLayerMisses; // was static @ :4669 (keyed by layer name)
    std::unordered_set<int>         soundVolErrored;  // was static @ :4955
    bool                            mathRandomProbed { false }; // was static @ :5096
    // one-shot guard for the first-flush unresolved-id scan.  After
    // dirty entries became keyed by resolved id (not name), the hot loop no
    // longer carries names, so the unknown-name diagnostic is produced once per
    // load by scanning _layerList; this latches it and dirtyLayerMisses holds
    // the reported names (per-load, cleared below).
    bool dirtyMissesScanned { false };

    void clear() {
        textErroredIds.clear();
        svErrored.clear();
        propErroredIds.clear();
        dirtyLayerMisses.clear();
        soundVolErrored.clear();
        mathRandomProbed   = false;
        dirtyMissesScanned = false;
    }
};

} // namespace scenebackend
