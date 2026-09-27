#pragma once

// The GUI-thread-safe read side of Scene::mediaTextureSlots. MainHandler (the
// parse-thread owner, declared with no header inside SceneWallpaper.cpp) fills
// this once per scene load; SceneWallpaper::getMediaTextureSize reads it from
// the GUI thread the same way getNodeNameToIdMap already reads
// MainHandler's m_node_name_to_id -- never handing the live Scene/imageParser
// across the thread boundary directly.

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Scene/include/Scene/Scene.h"

namespace wallpaper
{

class MediaTextureRegistry {
public:
    // Replaces the whole set (not a merge): a key present in the previous
    // scene but absent from this one must stop resolving, matching
    // scene-reload semantics -- a stale slot from the last wallpaper must
    // not answer a lookup for the new one.
    void reset(const std::vector<MediaTextureSlot>& slots) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_slots.clear();
        for (const auto& slot : slots) m_slots[slot.textureKey] = slot;
    }

    std::optional<MediaTextureSlot> get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto                        it = m_slots.find(key);
        if (it == m_slots.end()) return std::nullopt;
        return it->second;
    }

private:
    mutable std::mutex                                m_mutex;
    std::unordered_map<std::string, MediaTextureSlot> m_slots;
};

} // namespace wallpaper
