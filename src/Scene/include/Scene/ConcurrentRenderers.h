#pragma once
#include <mutex>
#include <unordered_map>

#include "Core/NoCopyMove.hpp"
#include "Type.hpp"

namespace wallpaper
{

// What every live wallpaper renderer in this process is currently drawing,
// keyed by renderer address.
//
// On a multi-monitor desktop each screen is its own wallpaper plasmoid with its
// own Vulkan device, all inside plasmashell and all sharing one GPU — and none
// of them can see the others.  A renderer deciding how expensive it may be
// (see Scene::autoMsaaSamples) has to know the whole process's output, or two
// screens will each independently conclude they can afford a sample count that
// together they cannot.
//
// Kept as an instantiable class with a process-wide Instance() rather than
// loose statics so tests can drive their own registry without leaking state
// between cases.
class ConcurrentRenderers : NoCopy {
public:
    ConcurrentRenderers() = default;

    static ConcurrentRenderers& Instance() {
        static ConcurrentRenderers inst;
        return inst;
    }

    // Record (or update) what `owner` is drawing.  Safe to call every frame;
    // the common case is an unchanged value.
    void Set(const void* owner, u64 pixels) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_pixels[owner] = pixels;
    }

    // Drop a renderer that is going away.  A screen unplug or a wallpaper
    // switch destroys one, and leaving its pixels behind would permanently
    // depress the quality tier for whichever renderer is left.
    void Remove(const void* owner) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_pixels.erase(owner);
    }

    // Pixels drawn by everyone except `owner`.
    u64 PixelsExcluding(const void* owner) const {
        std::lock_guard<std::mutex> lk(m_mutex);
        u64                         total = 0;
        for (const auto& [who, pixels] : m_pixels) {
            if (who != owner) total += pixels;
        }
        return total;
    }

private:
    mutable std::mutex                       m_mutex;
    std::unordered_map<const void*, u64>     m_pixels;
};

} // namespace wallpaper
