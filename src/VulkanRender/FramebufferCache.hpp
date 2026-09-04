#pragma once
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>

namespace wallpaper::vulkan
{

// Caches one framebuffer per attachment image view: create on miss, reuse on
// hit, clear() to invalidate (e.g. when the render pass changes on re-prepare,
// or when the swapchain destroys the views the keys name).
// Templated on Key/FB so the cache semantics are unit-testable without a device.
template<typename Key, typename FB>
class FramebufferCache {
public:
    // `make` returns std::nullopt when creation failed.  A failure is never
    // cached — caching it would bind a null framebuffer on every later frame
    // with nothing in the log to say why.  Returns nullptr in that case so the
    // caller can skip the pass for this frame and retry on the next one.
    template<typename Make>
    FB* getOrCreate(const Key& key, Make&& make) {
        auto it = m_map.find(key);
        if (it == m_map.end()) {
            std::optional<FB> fb = make();
            if (! fb.has_value()) return nullptr;
            it = m_map.emplace(key, std::move(*fb)).first;
        }
        return &it->second;
    }

    void        clear() { m_map.clear(); }
    std::size_t size() const { return m_map.size(); }
    bool        contains(const Key& key) const { return m_map.find(key) != m_map.end(); }

private:
    std::unordered_map<Key, FB> m_map;
};

} // namespace wallpaper::vulkan
