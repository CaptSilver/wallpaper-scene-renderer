#include <doctest.h>

#include "Particle/ParticleTrail.h"

using namespace wallpaper;

TEST_SUITE("ParticleTrailHistory") {

TEST_CASE("Init sets capacity and resets head/count") {
    ParticleTrailHistory h;
    h.Init(4);
    CHECK(h.Capacity() == 4u);
    CHECK(h.Count() == 0u);
    CHECK(h.MaxAge() == doctest::Approx(0.0f));
}

TEST_CASE("Push stores the point; At(0) returns the most recent one") {
    ParticleTrailHistory h;
    h.Init(4);
    ParticleTrailPoint a { { 1, 0, 0 }, 1.0f, 1.0f, { 1, 0, 0 }, 0.1f };
    h.Push(a);
    REQUIRE(h.Count() == 1u);
    CHECK(h.At(0).position.x() == doctest::Approx(1.f));
    CHECK(h.At(0).timestamp == doctest::Approx(0.1f));
}

TEST_CASE("At(1) returns older point, At(0) returns newer (distinct slots)") {
    // If the `m_head = (m_head + 1) % capacity` arithmetic were mutated, At(0)
    // would retrieve the wrong slot after two pushes.  Test both positions.
    ParticleTrailHistory h;
    h.Init(4);
    ParticleTrailPoint a {}, b {};
    a.position = { 1, 0, 0 };
    b.position = { 2, 0, 0 };
    h.Push(a);
    h.Push(b);
    REQUIRE(h.Count() == 2u);
    CHECK(h.At(0).position.x() == doctest::Approx(2.f)); // newest
    CHECK(h.At(1).position.x() == doctest::Approx(1.f)); // older
}

TEST_CASE("ring buffer overwrites oldest when capacity is exceeded") {
    // Pushing N+1 items into a buffer of capacity N: count stays at N and the
    // newest items replace the oldest.  Mutation `m_count < capacity` → `<=`
    // would let count grow to N+1, later hitting the At() assertion.
    ParticleTrailHistory h;
    h.Init(3);
    for (int i = 0; i < 5; i++) {
        ParticleTrailPoint p {};
        p.position = { (float)i, 0, 0 };
        h.Push(p);
    }
    CHECK(h.Count() == 3u);
    CHECK(h.At(0).position.x() == doctest::Approx(4.f)); // most recent (idx 4)
    CHECK(h.At(1).position.x() == doctest::Approx(3.f));
    CHECK(h.At(2).position.x() == doctest::Approx(2.f));
}

TEST_CASE("Push on zero-capacity buffer is safely ignored") {
    ParticleTrailHistory h;
    h.Init(0);
    h.Push(ParticleTrailPoint {});
    CHECK(h.Count() == 0u);
}

TEST_CASE("Clear resets head and count, keeps capacity") {
    ParticleTrailHistory h;
    h.Init(4);
    h.Push(ParticleTrailPoint {});
    h.Push(ParticleTrailPoint {});
    REQUIRE(h.Count() == 2u);
    h.Clear();
    CHECK(h.Count() == 0u);
    CHECK(h.Capacity() == 4u);
}

TEST_CASE("ActiveCount falls back to Count when max_age == 0") {
    ParticleTrailHistory h;
    h.Init(4, 0.0f);
    for (int i = 0; i < 3; i++) h.Push(ParticleTrailPoint {});
    CHECK(h.ActiveCount() == 3u);
}

TEST_CASE("ActiveCount trims trailing points older than max_age") {
    ParticleTrailHistory h;
    h.Init(5, 1.0f);
    // Push 5 points with timestamps 0, 1, 2, 3, 4.  Newest = 4.
    for (int i = 0; i < 5; i++) {
        ParticleTrailPoint p {};
        p.timestamp = (float)i;
        h.Push(p);
    }
    // Only points within 1.0s of newest (timestamp 4) qualify: t=4 and t=3.
    CHECK(h.ActiveCount() == 2u);
}

TEST_CASE("ActiveCount enforces at least 2 active when count allows") {
    // Guard "Always returns at least min(2, count)" — with big max_age gap
    // all but one point fall outside the window, but the minimum bounds the
    // result to 2 so the renderer always has a segment to draw.
    ParticleTrailHistory h;
    h.Init(5, 0.01f);
    for (int i = 0; i < 5; i++) {
        ParticleTrailPoint p {};
        p.timestamp = (float)i;
        h.Push(p);
    }
    CHECK(h.ActiveCount() >= 2u);
}

} // ParticleTrailHistory
