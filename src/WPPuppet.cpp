#include "WPPuppet.hpp"
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

        b.offset_trans = combined_tran[i].inverse();
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
    double global_blend = puppet_layer.m_global_blend;
    double total_blend  = puppet_layer.m_total_blend;

#ifndef WP_SUPPRESS_DEBUG_LOGGING
    static int _debug_frame_count = 0;
    if (_debug_frame_count == 0) {
        LOG_INFO("genFrame: global_blend=%.4f total_blend=%.4f layers=%zu bones=%zu",
                 global_blend,
                 total_blend,
                 puppet_layer.m_layers.size(),
                 bones.size());
        int matched = 0;
        for (auto& l : puppet_layer.m_layers)
            if (l.anim != nullptr) matched++;
        LOG_INFO("genFrame: %d/%zu layers have matched animations",
                 matched,
                 puppet_layer.m_layers.size());
    }
#endif

    puppet_layer.updateInterpolation(time);

    for (uint i = 0; i < m_final_affines.size(); i++) {
        const auto& bone   = bones[i];
        auto&       affine = m_final_affines[i];

        affine = Affine3f::Identity();
        assert(bone.parent < i || bone.noParent());
        const Affine3f parent =
            bone.noParent() ? Affine3f::Identity() : m_final_affines[bone.parent];

        Vector3f    trans { bone.transform.translation() * global_blend };
        Vector3f    scale { Vector3f::Ones() * global_blend };
        Quaterniond quat { Quaterniond::Identity() };
        Quaterniond ident { Quaterniond::Identity() };

        // double cur_blend { 0.0f };

        for (auto& layer : puppet_layer.m_layers) {
            auto& alayer = layer.anim_layer;
            if (layer.anim == nullptr || ! alayer.visible) continue;
            assert(i < layer.anim->bframes_array.size());
            if (i >= layer.anim->bframes_array.size()) continue;

            auto& info       = layer.interp_info;
            auto& frame_base = layer.anim->bframes_array[i].frames[(usize)0];
            auto& frame_a    = layer.anim->bframes_array[i].frames[(usize)info.frame_a];
            auto& frame_b    = layer.anim->bframes_array[i].frames[(usize)info.frame_b];

            double t     = info.t;
            double one_t = 1.0f - info.t;

            // break up the delta quaternions from the animation start quaternion
            // blend the starting quaternion using the reduced blending factor
            // blend the delta using the full blending factor
            auto frame_a_quat_delta = frame_a.quaternion * frame_base.quaternion.conjugate();
            auto frame_b_quat_delta = frame_b.quaternion * frame_base.quaternion.conjugate();
            quat *= frame_a_quat_delta.slerp(info.t, frame_b_quat_delta)
                        .slerp(1.0 - layer.anim_layer.blend, ident) *
                    frame_base.quaternion.slerp(1.0 - (layer.blend), ident);

            // break up the delta positions from the animation start position
            // blend the starting position using the reduced blending factor
            // blend the delta using the full blending factor
            auto frame_a_pos_delta = frame_a.position - frame_base.position;
            auto frame_b_pos_delta = frame_b.position - frame_base.position;
            trans += (layer.blend * frame_base.position) +
                     (layer.anim_layer.blend * (frame_a_pos_delta * one_t + frame_b_pos_delta * t));

            // break up the delta scales from the animation start scale
            // blend the starting scale using the reduced blending factor
            // blend the delta using the full blending factor
            auto& frame_a_scale_delta = frame_a.scale - frame_base.scale;
            auto& frame_b_scale_delta = frame_b.scale - frame_base.scale;
            scale += (layer.blend * frame_base.scale) +
                     (layer.anim_layer.blend *
                      (frame_a_scale_delta * one_t + frame_b_scale_delta * info.t));
        }
        affine.pretranslate(trans);
        affine.rotate(quat.slerp(global_blend, ident).cast<float>());
        affine.scale(scale);
        affine = parent * affine;
    }

#ifndef WP_SUPPRESS_DEBUG_LOGGING
    if (_debug_frame_count == 0 && ! m_final_affines.empty()) {
        auto bone0_trans = m_final_affines[0].translation();
        LOG_INFO("genFrame bone[0]: trans=(%.3f,%.3f,%.3f)",
                 bone0_trans.x(),
                 bone0_trans.y(),
                 bone0_trans.z());
        Eigen::Matrix4f t0 = (m_final_affines[0] * bones[0].offset_trans.matrix()).matrix();
        LOG_INFO("genFrame final bone[0] matrix diagonal: (%.4f, %.4f, %.4f, %.4f)",
                 t0(0, 0),
                 t0(1, 1),
                 t0(2, 2),
                 t0(3, 3));
    }
    _debug_frame_count++;
#endif

    for (uint i = 0; i < m_final_affines.size(); i++) {
        m_final_affines[i] *= bones[i].offset_trans.matrix();
    }
    return m_final_affines;
}

static constexpr void genInterpolationInfo(WPPuppet::Animation::InterpolationInfo& info,
                                           double& cur, u32 length, double frame_time,
                                           double max_time) {
    cur          = std::fmod(cur, max_time);
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
    double& blend       = m_global_blend;
    double& total_blend = m_total_blend;

    total_blend = 0.0;
    for (int i = 0; i < alayers.size(); i++) {
        if (alayers[i].visible) {
            total_blend += alayers[i].blend;
        }
    }

#ifndef WP_SUPPRESS_DEBUG_LOGGING
    LOG_INFO("puppet layer prepared: %zu scene layers, %zu mdl anims, total_blend=%.2f",
             alayers.size(),
             m_puppet->anims.size(),
             total_blend);
    for (usize i = 0; i < alayers.size(); i++) {
        LOG_INFO("  scene layer[%zu]: id=%d blend=%.2f rate=%.2f visible=%d",
                 i,
                 alayers[i].id,
                 alayers[i].blend,
                 alayers[i].rate,
                 (int)alayers[i].visible);
    }
#endif

    std::transform(
        alayers.rbegin(), alayers.rend(), m_layers.rbegin(), [&blend, this](const auto& layer) {
            double      cur_blend { 0.0f };
            const auto& anims = m_puppet->anims;

            auto it = std::find_if(anims.begin(), anims.end(), [&layer](auto& a) {
                return layer.id == a.id;
            });
            bool ok = it != anims.end() && layer.visible;
#ifndef WP_SUPPRESS_DEBUG_LOGGING
            LOG_INFO("  match layer id=%d: %s (cur_blend will be %.4f)",
                     layer.id,
                     ok ? "MATCHED" : "NOT FOUND",
                     ok ? layer.blend / m_total_blend : 0.0);
#endif

            double& total_blend = m_total_blend;

            if (ok) {
                if (total_blend > 1.0) {
                    cur_blend = layer.blend / total_blend;
                    blend     = 0.0;
                } else {
                    cur_blend = blend * layer.blend;
                    blend *= 1.0f - layer.blend;
                    blend = blend < 0.0f ? 0.0f : blend;
                }
            }

            return Layer {
                .anim_layer = layer,
                .blend      = cur_blend,
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
