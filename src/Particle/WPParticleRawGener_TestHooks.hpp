#pragma once
// Test-only probe interface for WPParticleRawGener rope generators.
// Lets the test suite drive the anon-namespace gen helpers directly and
// inspect the scratch-buffer capacity-at-entry — the observable that
// distinguishes function-local (capacity always 0 at entry) from
// `static thread_local` (capacity retained across calls on the same thread).
//
// Not part of the public ABI; included only by the .cpp that defines the
// gens and by the test_RopeScratchThreadLocal.cpp test.

#include <cstddef>
#include <span>
#include "Particle/Particle.h"
#include "Scene/SceneVertexArray.h"
#include "Interface/IParticleRawGener.h"
#include <Eigen/Core>

namespace wallpaper::test_hooks
{

// Captured at the START of each rope-gen call, before clear/resize.
// On the local-scratch path these are all 0 every call.
// On the thread_local path these grow once and retain across calls on
// the same thread.
struct RopeScratchProbe {
    std::size_t alive_capacity { 0 };
    std::size_t positions_capacity { 0 };
    std::size_t seg_lens_capacity { 0 };
    const void* alive_data { nullptr };
    const void* positions_data { nullptr };
    const void* seg_lens_data { nullptr };
};

// Returns the per-thread probe state captured by the most recent rope-gen
// call on this thread. Each thread has its own probe slot, so a thread
// that never called a rope gen sees a default-constructed probe.
RopeScratchProbe GetLastRopeScratchProbe() noexcept;

// Test wrappers around the anon-namespace gen helpers. Each calls the
// corresponding helper directly (with a no-op specOp + an empty WPGOption),
// then leaves the per-thread probe slot populated with the entry-capacity
// snapshot of the call.
std::size_t TestGenRopeParticleData(std::span<const Particle> particles,
                                    const Eigen::Vector3f& inst_pos, SceneVertexArray& sv,
                                    std::size_t start_idx, float anc_alpha = 1.0f);

std::size_t TestGenRopeParticleDataGS(std::span<const Particle> particles,
                                      const Eigen::Vector3f& inst_pos, SceneVertexArray& sv,
                                      std::size_t start_idx, float anc_alpha = 1.0f);

} // namespace wallpaper::test_hooks
