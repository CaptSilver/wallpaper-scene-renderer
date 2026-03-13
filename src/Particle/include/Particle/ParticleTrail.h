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
    float           timestamp { 0.0f };
};

class ParticleTrailHistory {
public:
    void Init(u32 capacity, float max_age = 0.0f) {
        m_points.resize(capacity);
        m_capacity = capacity;
        m_max_age  = max_age;
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

    u32   Count() const { return m_count; }
    u32   Capacity() const { return m_capacity; }
    float MaxAge() const { return m_max_age; }

    // Number of trail points within max_age of the newest point.
    // Falls back to Count() when max_age is not set.
    // Always returns at least min(2, count) so low fps never kills all segments.
    u32 ActiveCount() const {
        if (m_max_age <= 0.0f || m_count < 2) return m_count;
        float newest_time = At(0).timestamp;
        u32   min_active  = m_count < 2 ? m_count : 2;
        for (u32 i = 1; i < m_count; i++) {
            if (newest_time - At(i).timestamp > m_max_age)
                return std::max(min_active, i);
        }
        return m_count;
    }

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
    float                           m_max_age { 0.0f };
};

} // namespace wallpaper
