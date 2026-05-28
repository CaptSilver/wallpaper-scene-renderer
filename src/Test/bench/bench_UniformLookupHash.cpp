// Bench: uniform-name -> (offset, size) lookup hash.  Pins Round-5's
// std::unordered_map<std::string, BlockedUniform> choice; reverting any of
// the three hot-path maps (ShaderReflected::Block::member_map,
// WPShaderValueUpdater::m_nodeDataMap / m_nodeUniformInfoMap) to std::map
// shows up here as a 3-5x slowdown.

#include "nanobench.h"

#include <algorithm>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

struct BlockedUniform {
    unsigned int offset;
    unsigned int size;
    unsigned int num;
};

} // namespace

void register_UniformLookupHash(ankerl::nanobench::Bench& bench) {
    constexpr int N = 256;
    std::unordered_map<std::string, BlockedUniform> m;
    std::vector<std::string> keys;
    keys.reserve(N);
    for (int i = 0; i < N; ++i) {
        std::string k = "g_uniform_" + std::to_string(i);
        m.emplace(k, BlockedUniform { static_cast<unsigned>(i * 16), 16, 1 });
        keys.push_back(std::move(k));
    }
    std::mt19937 rng { 0xC0FFEEu };
    std::shuffle(keys.begin(), keys.end(), rng);

    bench.run("UniformLookupHash", [&] {
        long sum = 0;
        for (const auto& k : keys) {
            auto it = m.find(k);
            if (it != m.end()) sum += static_cast<long>(it->second.offset);
        }
        ankerl::nanobench::doNotOptimizeAway(sum);
    });
}
