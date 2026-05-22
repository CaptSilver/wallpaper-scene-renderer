#pragma once
#include <cstdint>

namespace wallpaper
{

// The matrix/VP uniform block (g_Model*, g_ModelViewProjection*,
// g_ViewProjection) is recomputed only when an input that feeds those
// matrices changed since the last upload for this (node,camera) pair.  This
// is the pure decision the per-pass uniform updater would otherwise re-run
// unconditionally every frame — including two double-precision 4x4 inversions
// (g_ModelMatrixInverse / g_ModelViewProjectionMatrixInverse).
//
// Splitting it out (mirroring WorldCacheGate.h / ScriptLoopGate.h) lets the
// policy be unit-tested without standing up a Vulkan CustomShaderPass.
//
// Returns true (recompute + re-upload) iff ANY of:
//   * firstUpload     — no cached value yet for this node+camera,
//   * parallaxActive  — parallax shifts the model matrix from live mouse
//                       input every frame; it must never be served from the
//                       static cache while enabled,
//   * shakeActive     — camera shake shifts the view-projection every frame
//                       for the global camera; must stay volatile while on,
//   * nodeEpoch  != cachedNodeEpoch — the node's world transform changed,
//   * vpEpoch    != cachedVpEpoch   — the camera's view-projection changed.
// parallaxActive / shakeActive keep the two volatile transform mutators out
// of the static fast-path (the hard preservation requirement).
inline bool uniformMatricesShouldRecompute(bool firstUpload, bool parallaxActive,
                                           bool shakeActive, uint64_t nodeEpoch,
                                           uint64_t cachedNodeEpoch, uint64_t vpEpoch,
                                           uint64_t cachedVpEpoch) {
    return firstUpload || parallaxActive || shakeActive || nodeEpoch != cachedNodeEpoch ||
           vpEpoch != cachedVpEpoch;
}

} // namespace wallpaper
