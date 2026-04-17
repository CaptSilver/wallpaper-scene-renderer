#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <span>
#include <Eigen/Geometry>

#include "Core/Literals.hpp"

namespace wallpaper
{

class WPPuppetLayer;

class WPPuppet {
public:
    enum class PlayMode
    {
        Loop,
        Mirror,
        Single
    };
    struct Bone {
        Eigen::Affine3f transform { Eigen::Affine3f::Identity() };
        uint32_t        parent { 0xFFFFFFFFu };

        bool noParent() const { return parent == 0xFFFFFFFFu; }
        // prepared
        Eigen::Affine3f offset_trans { Eigen::Affine3f::Identity() };
        /*
        Eigen::Vector3f world_axis_x;
        Eigen::Vector3f world_axis_y;
        Eigen::Vector3f world_axis_z;
        */
    };
    struct BoneFrame {
        Eigen::Vector3f position;
        Eigen::Vector3f angle;
        Eigen::Vector3f scale;

        // prepared
        Eigen::Quaterniond quaternion;
    };
    struct Animation {
        i32         id;
        double      fps;
        i32         length;
        PlayMode    mode;
        std::string name;

        struct BoneFrames {
            std::vector<BoneFrame> frames;
        };
        std::vector<BoneFrames> bframes_array;

        // Keyframe events attached to specific frames in the animation.
        // Emitted by the runtime when playback crosses the event's frame so
        // SceneScripts can react via their animationEvent(event, value) handler.
        struct Event {
            i32         frame { 0 };
            std::string name;
        };
        std::vector<Event> events;

        // prepared
        double max_time;
        double frame_time;
        struct InterpolationInfo {
            idx    frame_a;
            idx    frame_b;
            double t;
        };
        InterpolationInfo getInterpolationInfo(double* cur_time) const;
    };

public:
    std::vector<Bone>      bones;
    std::vector<Animation> anims;

    std::span<const Eigen::Affine3f> genFrame(WPPuppetLayer&, double time) noexcept;
    void                             prepared();

private:
    std::vector<Eigen::Affine3f> m_final_affines;
};

class WPPuppetLayer {
    friend class WPPuppet;

public:
    WPPuppetLayer();
    WPPuppetLayer(std::shared_ptr<WPPuppet>);
    ~WPPuppetLayer();

    bool hasPuppet() const { return (bool)m_puppet; };

    struct AnimationLayer {
        i32    id { 0 };
        double rate { 1.0f };
        double blend { 1.0f };
        bool   visible { true };
        double cur_time { 0.0f };
    };

    // Emitted when playback crosses an event keyframe during updateInterpolation.
    // Consumers (SceneBackend) drain these each tick and forward them to the
    // owning object's SceneScript animationEvent handler.
    struct PendingEvent {
        i32         frame { 0 };
        std::string name;
    };

    void prepared(std::span<AnimationLayer>);

    std::span<const Eigen::Affine3f> genFrame(double time) noexcept;

    void updateInterpolation(double time) noexcept;

    // Move pending events out and reset the internal queue.  Not thread-safe;
    // expected to be called on the render thread in the same tick as
    // genFrame/updateInterpolation.
    std::vector<PendingEvent> drainEvents() noexcept;

private:
    struct Layer {
        AnimationLayer                         anim_layer;
        double                                 blend;
        const WPPuppet::Animation*             anim { nullptr };
        WPPuppet::Animation::InterpolationInfo interp_info {};

        // Monotonically increasing playback position used for event crossing
        // detection.  Unaffected by Loop/Mirror wrap (unlike anim_layer.cur_time
        // which is folded inside the mode's period).
        double elapsed { 0.0 };

        // True until the first forward tick completes.  Used so events at
        // frame 0 fire at t=0 (inclusive) instead of only on loop wraps.
        bool first_fwd_tick { true };

        operator bool() const noexcept { return anim != nullptr; };
    };

    double m_global_blend { 1.0 };
    double m_total_blend { 0.0 };

    std::vector<Layer>        m_layers;
    std::vector<PendingEvent> m_pending_events;
    std::shared_ptr<WPPuppet> m_puppet;
};

} // namespace wallpaper
