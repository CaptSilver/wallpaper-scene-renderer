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

    TEST_CASE("ActiveCount enforces exactly 2 active when all older points stale") {
        // Only the newest point is within max_age.  ActiveCount still returns
        // 2 so the renderer always has a segment to draw — pinpoint the exact
        // value (not `>= 2`) to kill `i < m_count` boundary mutants in the
        // scan loop.
        ParticleTrailHistory h;
        h.Init(5, 0.01f);
        for (int i = 0; i < 5; i++) {
            ParticleTrailPoint p {};
            p.timestamp = (float)i;
            h.Push(p);
        }
        CHECK(h.ActiveCount() == 2u);
    }

    TEST_CASE("ActiveCount at count==2 returns 2 (no scan)") {
        // Two points, both within max_age — the scan loop starts at i=2 and
        // immediately falls out; still returns m_count=2.
        ParticleTrailHistory h;
        h.Init(4, 10.0f);
        ParticleTrailPoint p0 {}, p1 {};
        p0.timestamp = 0.0f;
        p1.timestamp = 1.0f;
        h.Push(p0);
        h.Push(p1);
        CHECK(h.ActiveCount() == 2u);
    }

    TEST_CASE("ActiveCount: all points fresh returns m_count") {
        // 3 points, all within max_age — loop exits naturally returning
        // m_count.  Kills `i < m_count` → `i <= m_count` (would assert At(3)).
        ParticleTrailHistory h;
        h.Init(4, 100.0f);
        for (int i = 0; i < 3; i++) {
            ParticleTrailPoint p {};
            p.timestamp = (float)i;
            h.Push(p);
        }
        CHECK(h.ActiveCount() == 3u);
    }

    TEST_CASE("ActiveCount: subtraction not addition in age check") {
        // newest=5, point i=2 timestamp=3. Original diff = 5-3 = 2, not > max_age=3.
        // Mutated to `newest + At(i).timestamp` = 5+3 = 8, which IS > 3, would
        // return i=2 early.  Original correctly keeps all points and returns m_count.
        ParticleTrailHistory h;
        h.Init(4, 3.0f);
        for (int i = 0; i < 4; i++) {
            ParticleTrailPoint p {};
            p.timestamp = (float)(i + 2); // 2, 3, 4, 5 — newest=5, all within 3s
            h.Push(p);
        }
        CHECK(h.ActiveCount() == 4u);
    }

    TEST_CASE("ActiveCount: exact boundary timestamp does NOT exclude (strict >)") {
        // newest=4, oldest=2.  newest - oldest = 2.0 exactly.  With max_age=2.0
        // and a strict `> max_age`, oldest is kept (not stale).  Mutated to
        // `>=` would classify it as stale and return 2 instead of 3.
        ParticleTrailHistory h;
        h.Init(3, 2.0f);
        ParticleTrailPoint a {}, b {}, c {};
        a.timestamp = 2.0f;
        b.timestamp = 3.0f;
        c.timestamp = 4.0f;
        h.Push(a);
        h.Push(b);
        h.Push(c);
        CHECK(h.ActiveCount() == 3u);
    }

    TEST_CASE("ActiveCount: max_age==0 with differing timestamps still returns full count") {
        // Pins the `m_max_age <= 0.0f` guard:  if the comparison were `<` instead
        // of `<=`, exact zero would fall through to the scan loop, where every
        // non-newest point is stale (newest - any > 0 with max_age=0) and the
        // scan returns 2 at i=2.  Strict `<=` keeps the early return so we
        // observe the unchanged Count().
        ParticleTrailHistory h;
        h.Init(5, 0.0f);
        for (int i = 0; i < 5; i++) {
            ParticleTrailPoint p {};
            p.timestamp = (float)i; // 0, 1, 2, 3, 4 — newest=4
            h.Push(p);
        }
        CHECK(h.ActiveCount() == 5u);
    }

    TEST_CASE("ActiveCount: empty trail returns 0 even when max_age is set") {
        // Tests the `m_count == 0` guard.  Mutating `==` to `!=` would call
        // At(0) on an empty trail (assert/UB).  Original returns 0 cleanly.
        ParticleTrailHistory h;
        h.Init(4, 1.0f);
        CHECK(h.ActiveCount() == 0u);
    }

    TEST_CASE("ActiveCount: middle stale point bisects forward — kills `i--` mutation") {
        // Tests the `i++` mutation (cxx_post_inc_to_post_dec).  With 4 points
        // pushed at timestamps 0, 1, 2, 3 and max_age=2:
        //   newest = At(0).timestamp = 3
        //   At(1) = 2, At(2) = 1, At(3) = 0
        //   At(2):   3 - 1 = 2  NOT > 2 (strict)  → fresh, scan continues
        //   At(3):   3 - 0 = 3  > 2               → stale, return 3.
        // With `i--` mutated, the loop runs i=2 (fresh), i=1 (fresh), i=0
        // (newest, fresh), then i wraps to UINT32_MAX which is NOT < m_count,
        // so the loop exits and returns m_count=4 — observably different.
        ParticleTrailHistory h;
        h.Init(4, 2.0f);
        for (int i = 0; i < 4; i++) {
            ParticleTrailPoint p {};
            p.timestamp = (float)i;
            h.Push(p);
        }
        CHECK(h.ActiveCount() == 3u);
    }

} // ParticleTrailHistory
