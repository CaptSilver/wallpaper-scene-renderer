#pragma once
#include <cstdint>
#include <vector>
#include <iostream>
#include <cstdint>
#include <array>

#include "Core/Literals.hpp"

namespace wallpaper
{
struct SpriteFrame {
    i32   imageId { 0 };
    float frametime { 0 };
    float x { 0 };
    float y { 0 };
    float width { 1 };
    float height { 1 };
    float rate { 1 }; // real h / w

    std::array<float, 2> xAxis { 1, 0 };
    std::array<float, 2> yAxis { 0, 1 };
};

class SpriteAnimation {
public:
    const auto& GetAnimateFrame(double newtime) {
        // Manual-frame mode pins the displayed frame regardless of elapsed
        // time — SceneScript writers use it via `thisLayer.getTextureAnimation()
        // .setFrame(N)` to lock a multi-frame sprite (button states with
        // idle/hover/selected frames; tool icons; etc.) onto a chosen frame.
        // Without this every tinted button on Game of Life (3453251764)
        // cycled through frames 0→1→2→0 at the texture's authored rate,
        // appearing to flash.
        if (m_manualFrame) {
            if (m_curFrame >= std::ssize(m_frames)) m_curFrame = 0;
            return m_frames.at((usize)m_curFrame);
        }
        if ((m_remainTime -= newtime) < 0.0f) {
            SwitchToNext();
            const auto& frame = m_frames.at((usize)m_curFrame);
            m_remainTime      = frame.frametime;
        }
        const auto& frame = m_frames.at((usize)m_curFrame);
        return frame;
    }
    const auto& GetCurFrame() const { return m_frames.at((usize)m_curFrame); }
    void        AppendFrame(const SpriteFrame& frame) { m_frames.push_back(frame); }

    usize numFrames() const { return m_frames.size(); }
    idx   curFrameIndex() const { return m_curFrame; }
    bool  isManualFrame() const { return m_manualFrame; }

    // Pin the sprite to frame `f`; clamps to [0, numFrames-1].  Auto-advance
    // is suppressed until ClearManualFrame() restores time-driven playback.
    void SetManualFrame(idx f) {
        if (m_frames.empty()) {
            m_curFrame    = 0;
            m_manualFrame = true;
            return;
        }
        if (f < 0)
            f = 0;
        else if (f >= std::ssize(m_frames))
            f = std::ssize(m_frames) - 1;
        m_curFrame    = f;
        m_manualFrame = true;
    }
    void ClearManualFrame() {
        m_manualFrame = false;
        m_remainTime  = m_frames.empty() ? 0.0 : m_frames.at((usize)m_curFrame).frametime;
    }

private:
    void SwitchToNext() {
        if (m_curFrame >= std::ssize(m_frames) - 1)
            m_curFrame = 0;
        else
            m_curFrame++;
    }
    idx    m_curFrame { 0 };
    double m_remainTime { 0 };
    bool   m_manualFrame { false };

    std::vector<SpriteFrame> m_frames;
};
} // namespace wallpaper
