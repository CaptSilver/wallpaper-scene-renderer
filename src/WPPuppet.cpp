#include "WPPuppet.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include "Utils/Logging.h"

using namespace wallpaper;
using namespace Eigen;

static Quaterniond ToQuaternion(Vector3f euler) {
    const std::array<Vector3d, 3> axis { Vector3d::UnitX(), Vector3d::UnitY(), Vector3d::UnitZ() };
    return AngleAxis<double>(euler.z(), axis[2]) * AngleAxis<double>(euler.y(), axis[1]) *
           AngleAxis<double>(euler.x(), axis[0]);
};

void WPPuppet::prepared() {
    std::vector<Affine3f> combined_tran(bones.size());
    for (uint i = 0; i < bones.size(); i++) {
        auto& b = bones[i];
        combined_tran[i] =
            (b.noParent() ? Affine3f::Identity() : combined_tran[b.parent]) * b.transform;

        b.world_transform = combined_tran[i];
        b.offset_trans    = combined_tran[i].inverse();
        /*
        b.world_axis_x = (b.offset_trans.linear() *
        Vector3f::UnitX()).normalized(); b.world_axis_y =
        (b.offset_trans.linear() * Vector3f::UnitY()).normalized();
        b.world_axis_z = (b.offset_trans.linear() *
        Vector3f::UnitZ()).normalized();
        */
    }
    for (auto& anim : anims) {
        anim.frame_time = 1.0f / anim.fps;
        anim.max_time   = anim.length / anim.fps;
        for (auto& b : anim.bframes_array) {
            for (auto& f : b.frames) {
                f.quaternion = ToQuaternion(f.angle);
            }
        }
    }

    m_final_affines.resize(bones.size());
}

std::span<const Eigen::Affine3f> WPPuppet::genFrame(WPPuppetLayer& puppet_layer,
                                                    double         time) noexcept {
    double total_blend = puppet_layer.m_total_blend;

    puppet_layer.updateInterpolation(time);

    const Quaterniond ident { Quaterniond::Identity() };

    for (uint i = 0; i < m_final_affines.size(); i++) {
        const auto& bone   = bones[i];
        auto&       affine = m_final_affines[i];

        affine = Affine3f::Identity();
        assert(bone.parent < i || bone.noParent());
        const Affine3f parent =
            bone.noParent() ? Affine3f::Identity() : m_final_affines[bone.parent];

        // Wallpaper Engine stacks animation layers bottom-to-top onto the bind
        // (rest) pose, honouring each layer's `additive` flag:
        //   - non-additive: blend OVER the running pose toward the layer's
        //     absolute pose by the layer weight, so a full-weight layer fully
        //     replaces what is below it (the topmost full-weight layer wins).
        //   - additive: add the layer's frame-relative delta, scaled by weight,
        //     on top of the running pose.
        // The previous code ignored `additive` and, for total weight > 1,
        // *averaged* the layers' frame-0 poses.  That detached the hair on
        // Weathering With You (2558523891) — two full-weight non-additive
        // layers whose second layer starts with the hair swung out should let
        // that layer win, not blend halfway into it.  Starting from the bind
        // pose and blending over / adding per the flag matches WE.
        Matrix3f bindR, bindS;
        bone.transform.computeRotationScaling(&bindR, &bindS);
        Vector3f    trans { bone.transform.translation() };
        Vector3f    scale { bindS.diagonal() };
        Quaterniond quat { Quaterniond(bindR.cast<double>()) };

        for (auto& layer : puppet_layer.m_layers) {
            auto& alayer = layer.anim_layer;
            if (layer.anim == nullptr || ! alayer.visible) continue;
            assert(i < layer.anim->bframes_array.size());
            if (i >= layer.anim->bframes_array.size()) continue;

            auto& info       = layer.interp_info;
            auto& frame_base = layer.anim->bframes_array[i].frames[(usize)0];
            auto& frame_a    = layer.anim->bframes_array[i].frames[(usize)info.frame_a];
            auto& frame_b    = layer.anim->bframes_array[i].frames[(usize)info.frame_b];

            float  t     = (float)info.t;
            float  one_t = 1.0f - t;
            double w     = std::clamp(alayer.blend, 0.0, 1.0);

            // This layer's interpolated absolute pose for bone i.
            Vector3f    pos_i = frame_a.position * one_t + frame_b.position * t;
            Vector3f    scale_i { frame_a.scale * one_t + frame_b.scale * t };
            Quaterniond quat_i = frame_a.quaternion.slerp(info.t, frame_b.quaternion);

            if (alayer.additive) {
                trans += (float)w * (pos_i - frame_base.position);
                scale += (float)w * (scale_i - frame_base.scale);
                Quaterniond dq = quat_i * frame_base.quaternion.conjugate();
                quat           = quat * ident.slerp(w, dq);
            } else {
                trans = trans * (1.0f - (float)w) + pos_i * (float)w;
                scale = scale * (1.0f - (float)w) + scale_i * (float)w;
                quat  = quat.slerp(w, quat_i);
            }
        }
        affine.pretranslate(trans);
        affine.rotate(quat.cast<float>());
        affine.scale(scale);
        affine = parent * affine;
    }

#ifndef WP_SUPPRESS_DEBUG_LOGGING
    if (! m_logged_bones_frame0 && ! m_final_affines.empty()) {
        // One-shot per-puppet frame-0 diagnostic.  Reports bone count,
        // blend state, matched animations, and whether the animated bone
        // world (m_final_affines[i], pre-offset_trans) diverges from the
        // bind pose bones[i].world_transform.  At frame 0 a correctly
        // anchored puppet sits at bind pose, so both max_dX and max_dY are
        // ≈ 0; a large divergence means an active animation's frame-0 differs
        // from bind and is pulling bones off the rest pose.  Measure BOTH
        // axes — a hair-sway animation displaces bones horizontally, which a
        // Y-only check silently misses.
        double max_dY = 0.0, max_dX = 0.0;
        int    max_dX_bone = -1;
        for (uint i = 0; i < bones.size(); i++) {
            double dY =
                m_final_affines[i].translation().y() - bones[i].world_transform.translation().y();
            double dX =
                m_final_affines[i].translation().x() - bones[i].world_transform.translation().x();
            if (std::abs(dY) > std::abs(max_dY)) max_dY = dY;
            if (std::abs(dX) > std::abs(max_dX)) {
                max_dX      = dX;
                max_dX_bone = (int)i;
            }
        }
        int matched = 0;
        for (auto& l : puppet_layer.m_layers)
            if (l.anim != nullptr) matched++;
        LOG_INFO("genFrame frame0: bones=%zu total_blend=%.2f "
                 "matched_anims=%d/%zu max_dY=%.2f max_dX=%.2f@bone%d",
                 bones.size(),
                 total_blend,
                 matched,
                 puppet_layer.m_layers.size(),
                 max_dY,
                 max_dX,
                 max_dX_bone);
        m_logged_bones_frame0 = true;
    }
#endif

    for (uint i = 0; i < m_final_affines.size(); i++) {
        m_final_affines[i] *= bones[i].offset_trans.matrix();
    }
    return m_final_affines;
}

static constexpr void genInterpolationInfo(WPPuppet::Animation::InterpolationInfo& info,
                                           double& cur, u32 length, double frame_time,
                                           double max_time) {
    cur = std::fmod(cur, max_time);
    // Reverse playback (negative animation-layer rate, e.g. Hoshi-Tele 幽's
    // leg "swing" driven at a negative rate): C++ fmod keeps the dividend's
    // sign, so cur goes negative and `(uint)_rate` below is undefined behaviour
    // — it samples garbage frames, contorting the limb and scaling it wrong.
    // Wrap back into [0, max_time) so reverse animations sample real frames.
    if (cur < 0.0) cur += max_time;
    double _rate = cur / frame_time;

    info.frame_a = ((uint)_rate) % length;
    info.frame_b = (info.frame_a + 1) % length;
    info.t       = _rate - (double)info.frame_a;
}

WPPuppet::Animation::InterpolationInfo
WPPuppet::Animation::getInterpolationInfo(double* cur_time) const {
    InterpolationInfo _info;
    auto&             _cur_time = *cur_time;

    if (mode == PlayMode::Single) {
        // Play once and hold the last frame
        if (_cur_time >= max_time) {
            _cur_time     = max_time;
            _info.frame_a = (idx)(length - 1);
            _info.frame_b = _info.frame_a;
            _info.t       = 0.0;
        } else {
            genInterpolationInfo(_info, _cur_time, (u32)length, frame_time, max_time);
        }
    } else if (mode == PlayMode::Loop) {
        genInterpolationInfo(_info, _cur_time, (u32)length, frame_time, max_time);
    } else if (mode == PlayMode::Mirror) {
        const auto _get_frame = [this](auto f) {
            return f >= length ? (length - 1) - (f - length) : f;
        };
        genInterpolationInfo(_info, _cur_time, (u32)length * 2, frame_time, max_time * 2.0f);
        _info.frame_a = _get_frame(_info.frame_a);
        _info.frame_b = _get_frame(_info.frame_b);
    }

    return _info;
}

void WPPuppetLayer::prepared(std::span<AnimationLayer> alayers) {
    m_layers.resize(alayers.size());

    // Sum of visible layer weights — informational only (the per-layer
    // blend-over in genFrame needs no global normalization).
    m_total_blend = 0.0;
    for (const auto& l : alayers)
        if (l.visible) m_total_blend += l.blend;

#ifndef WP_SUPPRESS_DEBUG_LOGGING
    LOG_INFO("puppet layer prepared: %zu scene layers, %zu mdl anims, total_blend=%.2f",
             alayers.size(),
             m_puppet->anims.size(),
             m_total_blend);
    for (usize i = 0; i < alayers.size(); i++) {
        LOG_INFO("  scene layer[%zu]: id=%d blend=%.2f rate=%.2f visible=%d additive=%d",
                 i,
                 alayers[i].id,
                 alayers[i].blend,
                 alayers[i].rate,
                 (int)alayers[i].visible,
                 (int)alayers[i].additive);
    }
#endif

    // Match each scene layer to its mdl animation by id, preserving scene order
    // (bottom-to-top) so genFrame stacks them correctly.
    const auto& anims = m_puppet->anims;
    std::transform(alayers.begin(), alayers.end(), m_layers.begin(), [&anims](const auto& layer) {
        auto it = std::find_if(anims.begin(), anims.end(), [&layer](auto& a) {
            return layer.id == a.id;
        });
        bool ok = it != anims.end() && layer.visible;
        return Layer {
            .anim_layer = layer,
            .anim       = ok ? std::addressof(*it) : nullptr,
        };
    });
}

std::span<const Eigen::Affine3f> WPPuppetLayer::genFrame(double time) noexcept {
    return m_puppet->genFrame(*this, time);
}

namespace
{

// Append every event fire time in the half-open interval (prev, cur] for an
// event whose period-relative fire offset(s) are `offsets` and whose period
// is `period`.  Used for Loop (one offset) and Mirror (two offsets per period).
void collectPeriodicFires(double prev, double cur, double period,
                          std::initializer_list<double> offsets, std::vector<double>& out) {
    if (period <= 0.0 || cur <= prev) return;
    // Cap loop span so a pathologically huge delta (e.g. first tick after a
    // stall) cannot explode into millions of fires for a fast-loop anim.
    constexpr int64_t kMaxPeriodsPerTick = 256;
    int64_t           k_start            = (int64_t)std::floor(prev / period);
    int64_t           k_end              = (int64_t)std::floor(cur / period);
    if (k_end - k_start > kMaxPeriodsPerTick) {
        k_start = k_end - kMaxPeriodsPerTick;
    }
    for (int64_t k = k_start; k <= k_end; k++) {
        for (double off : offsets) {
            double t = (double)k * period + off;
            if (t > prev && t <= cur) out.push_back(t);
        }
    }
}

} // namespace

void WPPuppetLayer::updateInterpolation(double time) noexcept {
    for (auto& layer : m_layers) {
        if (! layer) continue;

        double advance = time * layer.anim_layer.rate;

        // Only forward playback dispatches keyframe events (matches WE
        // behavior — reverse/pause do not re-fire).
        if (advance > 0.0 && ! layer.anim->events.empty()) {
            const auto& anim    = *layer.anim;
            double      prev    = layer.elapsed;
            double      next    = prev + advance;
            double      max_t   = anim.max_time;
            double      frame_t = anim.frame_time;

            // On the very first forward tick, treat the lower bound as
            // inclusive so events placed at frame 0 fire at t=0 (matching
            // WE's "fires when playback reaches this frame" semantic).
            // Later ticks use half-open (prev, next] to avoid re-firing.
            bool   inclusive_low = layer.first_fwd_tick;
            double prev_bound    = inclusive_low
                                       ? std::nextafter(prev, -std::numeric_limits<double>::infinity())
                                       : prev;

            std::vector<double> fires;
            for (const auto& evt : anim.events) {
                double event_time = (double)evt.frame * frame_t;
                if (event_time < 0.0) event_time = 0.0;
                if (event_time > max_t) event_time = max_t;

                fires.clear();
                switch (anim.mode) {
                case WPPuppet::PlayMode::Single:
                    if (event_time > prev_bound && event_time <= next) {
                        fires.push_back(event_time);
                    }
                    break;
                case WPPuppet::PlayMode::Loop:
                    collectPeriodicFires(prev_bound, next, max_t, { event_time }, fires);
                    break;
                case WPPuppet::PlayMode::Mirror: {
                    double period = max_t * 2.0;
                    collectPeriodicFires(
                        prev_bound, next, period, { event_time, period - event_time }, fires);
                    break;
                }
                }
                for (size_t i = 0; i < fires.size(); i++) {
                    m_pending_events.push_back({ evt.frame, evt.name });
                }
            }
            layer.first_fwd_tick = false;
        }

        if (advance >= 0.0) {
            layer.elapsed += advance;
            // For Single, don't let elapsed drift forever past max_time — the
            // interpolation itself clamps there, so freeze the event window too.
            if (layer.anim->mode == WPPuppet::PlayMode::Single &&
                layer.elapsed > layer.anim->max_time) {
                layer.elapsed = layer.anim->max_time;
            }
        } else {
            // Negative advance: keep elapsed bounded below at 0 (e.g. rewind
            // during scene reload transients).
            layer.elapsed += advance;
            if (layer.elapsed < 0.0) layer.elapsed = 0.0;
        }

        layer.anim_layer.cur_time += advance;
        layer.interp_info = layer.anim->getInterpolationInfo(&(layer.anim_layer.cur_time));
    }
}

std::vector<WPPuppetLayer::PendingEvent> WPPuppetLayer::drainEvents() noexcept {
    std::vector<PendingEvent> out;
    out.swap(m_pending_events);
    return out;
}

WPPuppetLayer::WPPuppetLayer(std::shared_ptr<WPPuppet> pup): m_puppet(pup) {}
WPPuppetLayer::WPPuppetLayer()  = default;
WPPuppetLayer::~WPPuppetLayer() = default;
