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
    // For trails with ≥2 points the result is always at least 2 (the two
    // newest are kept), so low fps never kills all segments — the caller
    // draws line segments and needs ≥2 points to render anything.  For
    // trails with 0 or 1 points we return Count() unchanged.
    //
    // Because timestamps are monotonic (Push is chronological, At(0) is the
    // newest), staleness is monotonic: if point i is stale, so is i+1.  We
    // therefore scan from i=2 — the first stale index we find is already the
    // answer, no separate `min(2, i)` clamp required.
    //
    // Implementation note (mutation testing): the empty-trail guard uses
    // `m_count == 0` (rather than `m_count < 2`) because at m_count==2 the
    // scan loop body does not execute — both `<` and `<=` paths return 2
    // and the boundary mutation is equivalent.  Splitting the guard into
    // an explicit zero check makes `==`/`!=` mutations observable: a
    // mutated guard tries to call At(0) on an empty trail and trips the
    // debug assert.
    u32 ActiveCount() const {
        if (m_max_age <= 0.0f) return m_count;
        if (m_count == 0) return 0;
        const float newest_time = At(0).timestamp;
        for (u32 i = 2; i < m_count; i++) {
            if (newest_time - At(i).timestamp > m_max_age) return i;
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
