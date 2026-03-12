#pragma once

#include <Eigen/Core>
#include <vector>
#include <cassert>
#include "Core/Literals.hpp"

namespace wallpaper
{

struct ParticleTrailPoint {
    Eigen::Vector3f position { 0, 0, 0 };
    float           size { 0 };
    float           alpha { 0 };
    Eigen::Vector3f color { 1, 1, 1 };
};

class ParticleTrailHistory {
public:
    void Init(u32 capacity) {
        m_points.resize(capacity);
        m_capacity = capacity;
        m_head     = 0;
        m_count    = 0;
    }

    void Push(const ParticleTrailPoint& pt) {
        if (m_capacity == 0) return;
        m_points[m_head] = pt;
        m_head           = (m_head + 1) % m_capacity;
        if (m_count < m_capacity) m_count++;
    }

    void Clear() {
        m_count = 0;
        m_head  = 0;
    }

    u32 Count() const { return m_count; }
    u32 Capacity() const { return m_capacity; }

    // At(0) = newest, At(count-1) = oldest
    const ParticleTrailPoint& At(u32 i) const {
        assert(i < m_count);
        u32 idx = (m_head + m_capacity - 1 - i) % m_capacity;
        return m_points[idx];
    }

private:
    std::vector<ParticleTrailPoint> m_points;
    u32                             m_capacity { 0 };
    u32                             m_head { 0 };
    u32                             m_count { 0 };
};

} // namespace wallpaper
