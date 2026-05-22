#pragma once
#include <cstddef>
#include <unordered_map>

namespace wallpaper::vulkan
{

// Caches one framebuffer per attachment image view: create on miss, reuse on
// hit, clear() to invalidate (e.g. when the render pass changes on re-prepare).
// Templated on Key/FB so the cache semantics are unit-testable without a device.
template<typename Key, typename FB>
class FramebufferCache {
public:
    template<typename Make>
    FB& getOrCreate(const Key& key, Make&& make) {
        auto it = m_map.find(key);
        if (it == m_map.end()) {
            it = m_map.emplace(key, make()).first;
        }
        return it->second;
    }

    void        clear() { m_map.clear(); }
    std::size_t size() const { return m_map.size(); }
    bool        contains(const Key& key) const { return m_map.find(key) != m_map.end(); }

private:
    std::unordered_map<Key, FB> m_map;
};

} // namespace wallpaper::vulkan
