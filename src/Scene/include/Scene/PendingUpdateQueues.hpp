#pragma once
// PendingUpdateQueues — the render-thread bridge's pending-update state, lifted
// out of RenderHandler (SceneWallpaper.cpp) so its enqueue/merge/drain semantics
// are unit- and race-testable without a Vulkan link.
//
// It owns the three per-tick mutex groups exactly as the live queue did:
//   - m_text_mutex     : text / pointsize / text-style updates
//   - m_color_mutex    : per-layer colour-script writes
//   - m_property_mutex : transform / visible / alpha / particle-rate / effect
//                        visibility / material writes / sprite frame, plus all
//                        scene-level optionals (clear/bloom/camera/ambient/
//                        skylight) and the light vectors.
//
// The setters merge per-tick the same way as before (last-write-wins per key,
// per-field text-style merge, vectors append in order).  Draining stays on the
// caller: withTextLocked / withColorLocked / withPropertyLocked take the group
// mutex and hand the caller mutable refs to that group's maps, so the render
// thread iterates + applies + clears while HOLDING the lock — identical
// lock-hold semantics to the original drain (Vulkan work stayed under the lock).
//
// Kept GPU-free and decoupled from SceneWallpaper.hpp: applyLayerBatch takes a
// queue-local LayerBatchEntry POD; RenderHandler translates
// SceneWallpaper::LayerBatchUpdate -> LayerBatchEntry at the call boundary.  The
// four scene-level update element structs (CameraLookAtUpdate / LightColorUpdate
// / LightScalarUpdate / LightPositionUpdate) live here since this struct owns
// their vectors/optionals.
#include "Core/Literals.hpp"
#include "Scene/TextStyleMerge.hpp"

#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wallpaper
{

struct PendingUpdateQueues {
    PendingUpdateQueues()                                      = default;
    PendingUpdateQueues(const PendingUpdateQueues&)            = delete;
    PendingUpdateQueues& operator=(const PendingUpdateQueues&) = delete;
    PendingUpdateQueues(PendingUpdateQueues&&)                 = delete;
    PendingUpdateQueues& operator=(PendingUpdateQueues&&)      = delete;

    // ── Scene-level update element types (owned by the vectors/optionals) ─────
    struct CameraLookAtUpdate {
        std::array<double, 3> eye, center, up;
    };
    struct LightColorUpdate {
        i32                  index;
        std::array<float, 3> color;
    };
    struct LightScalarUpdate {
        i32   index;
        float value;
    };
    struct LightPositionUpdate {
        i32                  index;
        std::array<float, 3> position;
    };

    // ── applyLayerBatch input (queue-local; RenderHandler translates from
    //    SceneWallpaper::LayerBatchUpdate).  Flags mirror the JS DIRTY_STRIDE
    //    F_* bitmask. ──────────────────────────────────────────────────────────
    static constexpr u32 F_ORIGIN = 1, F_SCALE = 2, F_ANGLES = 4, F_VISIBLE = 8, F_ALPHA = 16;
    struct LayerBatchEntry {
        i32                  id;
        u32                  flags;
        std::array<float, 3> origin;
        std::array<float, 3> scale;
        std::array<float, 3> angles;
        float                alpha;
        u8                   visible;
    };

    // ── Text group (m_text_mutex) ─────────────────────────────────────────────
    void setTextUpdate(i32 id, const std::string& text) {
        std::lock_guard<std::mutex> lock(m_text_mutex);
        m_pending_text_updates[id] = text;
    }
    void setTextPointsize(i32 id, float pointsize) {
        std::lock_guard<std::mutex> lock(m_text_mutex);
        m_pending_pointsize_updates[id] = pointsize;
    }
    void setTextStyle(i32 id, std::string halign, std::string valign, std::string fontName) {
        std::lock_guard<std::mutex> lock(m_text_mutex);
        mergeTextStyle(m_pending_text_style_updates[id], halign, valign, fontName);
    }

    // ── Color group (m_color_mutex) ───────────────────────────────────────────
    void setColorUpdate(i32 id, float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_color_mutex);
        m_pending_color_updates[id] = { r, g, b };
    }

    // ── Property group (m_property_mutex) ─────────────────────────────────────
    void setNodeTransform(i32 id, const std::string& property, float x, float y, float z) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        auto                        key  = std::make_pair(id, property);
        m_pending_transform_updates[key] = { x, y, z };
    }
    void setNodeVisible(i32 id, bool visible) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_visible_updates[id] = visible;
    }
    void setNodeAlpha(i32 id, float alpha) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_alpha_updates[id] = alpha;
    }
    void setParticleRate(i32 id, float rate) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_particle_rate[id] = rate;
    }
    void setEffectVisible(i32 nodeId, i32 effectIndex, bool visible) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_effect_visible.emplace_back(nodeId, effectIndex, visible);
    }
    void setMaterialValue(i32 nodeId, std::string name, std::vector<float> floats) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_material_values.emplace_back(nodeId, std::move(name), std::move(floats));
    }
    void setEffectMaterialValue(i32 nodeId, i32 effectIdx, std::string name,
                                std::vector<float> floats) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_effect_material_values.emplace_back(
            nodeId, effectIdx, std::move(name), std::move(floats));
    }
    void setLayerSpriteFrame(i32 nodeId, bool wantsManual, i32 frameIdx) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_sprite_frame[nodeId] = { wantsManual, frameIdx };
    }

    // Scene-level property setters (m_property_mutex)
    void setClearColor(float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_clear_color = std::array { r, g, b };
    }
    void setBloomStrength(float v) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_bloom_strength = v;
    }
    void setBloomThreshold(float v) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_bloom_threshold = v;
    }
    void setCameraFov(float v) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_camera_fov = v;
    }
    void setCameraLookAt(float ex, float ey, float ez, float cx, float cy, float cz, float ux,
                         float uy, float uz) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_camera_lookat = CameraLookAtUpdate { { (double)ex, (double)ey, (double)ez },
                                                       { (double)cx, (double)cy, (double)cz },
                                                       { (double)ux, (double)uy, (double)uz } };
    }
    void setAmbientColor(float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_ambient_color = std::array { r, g, b };
    }
    void setSkylightColor(float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_skylight_color = std::array { r, g, b };
    }
    void setLightColor(i32 index, float r, float g, float b) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_light_colors.push_back({ index, { r, g, b } });
    }
    void setLightRadius(i32 index, float v) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_light_radii.push_back({ index, v });
    }
    void setLightIntensity(i32 index, float v) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_light_intensities.push_back({ index, v });
    }
    void setLightPosition(i32 index, float x, float y, float z) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        m_pending_light_positions.push_back({ index, { x, y, z } });
    }

    // Batched layer-update apply: one property-mutex lock for the whole batch.
    // Unfolds each entry into transform/visible/alpha under the same bitmask the
    // original applyLayerBatch used (F_ORIGIN..F_ALPHA).
    void applyLayerBatch(const std::vector<LayerBatchEntry>& batch) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        for (const auto& e : batch) {
            if (e.flags & F_ORIGIN) {
                m_pending_transform_updates[{ e.id, std::string("origin") }] = { e.origin[0],
                                                                                 e.origin[1],
                                                                                 e.origin[2] };
            }
            if (e.flags & F_SCALE) {
                m_pending_transform_updates[{ e.id, std::string("scale") }] = { e.scale[0],
                                                                                e.scale[1],
                                                                                e.scale[2] };
            }
            if (e.flags & F_ANGLES) {
                m_pending_transform_updates[{ e.id, std::string("angles") }] = { e.angles[0],
                                                                                 e.angles[1],
                                                                                 e.angles[2] };
            }
            if (e.flags & F_VISIBLE) {
                m_pending_visible_updates[e.id] = e.visible != 0;
            }
            if (e.flags & F_ALPHA) {
                m_pending_alpha_updates[e.id] = e.alpha;
            }
        }
    }

    // ── Drains — caller iterates+applies+clears WHILE HOLDING the group lock ───
    // These preserve the original lock-hold semantics: the render thread does its
    // apply (including Vulkan reuploadTexture) inside the callback, under the
    // group mutex, exactly as the inline drains did.
    template<class Fn>
    void withTextLocked(Fn&& fn) {
        std::lock_guard<std::mutex> lock(m_text_mutex);
        fn(m_pending_text_updates, m_pending_pointsize_updates, m_pending_text_style_updates);
    }
    template<class Fn>
    void withColorLocked(Fn&& fn) {
        std::lock_guard<std::mutex> lock(m_color_mutex);
        fn(m_pending_color_updates);
    }
    // The property group holds ~18 maps/vectors/optionals; passing the whole
    // queue by ref keeps the moved-in drain body verbatim (q.m_pending_*).
    template<class Fn>
    void withPropertyLocked(Fn&& fn) {
        std::lock_guard<std::mutex> lock(m_property_mutex);
        fn(*this);
    }

    // ── State — public so the withXLocked callbacks (the moved drain bodies)
    //    touch them directly. ────────────────────────────────────────────────
    std::mutex                                      m_text_mutex;
    std::unordered_map<i32, std::string>            m_pending_text_updates;
    std::unordered_map<i32, float>                  m_pending_pointsize_updates;
    std::unordered_map<i32, PendingTextStyleUpdate> m_pending_text_style_updates;

    std::mutex                                    m_color_mutex;
    std::unordered_map<i32, std::array<float, 3>> m_pending_color_updates;

    std::mutex                                                    m_property_mutex;
    std::map<std::pair<i32, std::string>, std::array<float, 3>>   m_pending_transform_updates;
    std::unordered_map<i32, bool>                                 m_pending_visible_updates;
    std::unordered_map<i32, float>                                m_pending_alpha_updates;
    std::unordered_map<i32, float>                                m_pending_particle_rate;
    std::vector<std::tuple<i32, i32, bool>>                       m_pending_effect_visible;
    std::vector<std::tuple<i32, std::string, std::vector<float>>> m_pending_material_values;
    std::vector<std::tuple<i32, i32, std::string, std::vector<float>>>
                                                  m_pending_effect_material_values;
    std::unordered_map<i32, std::pair<bool, i32>> m_pending_sprite_frame;

    std::optional<std::array<float, 3>> m_pending_clear_color;
    std::optional<float>                m_pending_bloom_strength;
    std::optional<float>                m_pending_bloom_threshold;
    std::optional<float>                m_pending_camera_fov;
    std::optional<CameraLookAtUpdate>   m_pending_camera_lookat;
    std::optional<std::array<float, 3>> m_pending_ambient_color;
    std::optional<std::array<float, 3>> m_pending_skylight_color;
    std::vector<LightColorUpdate>       m_pending_light_colors;
    std::vector<LightScalarUpdate>      m_pending_light_radii;
    std::vector<LightScalarUpdate>      m_pending_light_intensities;
    std::vector<LightPositionUpdate>    m_pending_light_positions;
};

} // namespace wallpaper
