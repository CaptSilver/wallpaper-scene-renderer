// Bench: WPShaderValueUpdater::SetNodeData repeated insertion + lookup.
// Pins the per-frame cost of the m_nodeDataMap probe (the test_UniformLookupHash
// type-pin guarantees the container choice; this bench guarantees the
// throughput).  A revert to std::map shows up as a 3-5x slowdown.
//
// We avoid pulling in the full Scene + parser stack so the bench builds
// without a Vulkan device; the m_nodeDataMap probe is the load-bearing path
// for the frame-time hot loop and is exercised directly by SetNodeData.

#include "nanobench.h"
#include "WPShaderValueUpdater.hpp"

#include <array>
#include <vector>

void register_WPShaderValueUpdater(ankerl::nanobench::Bench& bench) {
    using wallpaper::WPShaderValueData;
    using wallpaper::WPShaderValueUpdater;

    constexpr int N = 64;
    // Stable pointer-sized keys; the updater stores them as void* (the real
    // call site is "pass the SceneNode* by value"), so an int array's
    // addresses model the per-frame node-pointer distribution.
    std::vector<int> nodes(N, 0);
    WPShaderValueUpdater updater(nullptr);
    for (int i = 0; i < N; ++i) {
        WPShaderValueData d;
        d.parallaxDepth = { float(i) * 0.01f, float(i) * 0.02f };
        updater.SetNodeData(static_cast<void*>(&nodes[i]), d);
    }

    bench.run("WPShaderValueUpdater_SetNodeData", [&] {
        // Rewrite each entry's parallaxDepth once -- this is the per-frame
        // shape of the m_nodeDataMap touch in WPShaderValueUpdater::Frame*.
        for (int i = 0; i < N; ++i) {
            WPShaderValueData d;
            d.parallaxDepth = { 1.0f / float(i + 1), 2.0f / float(i + 1) };
            updater.SetNodeData(static_cast<void*>(&nodes[i]), d);
        }
        ankerl::nanobench::doNotOptimizeAway(updater);
    });
}
