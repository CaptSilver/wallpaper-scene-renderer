#include <doctest.h>

#include "Particle/ParticleEmitter.h"
#include "Particle/ParticleModify.h"
#include "Particle/Particle.h"

#include <vector>

using namespace wallpaper;

// All tests use emitSpeed values that yield an exactly-representable emitDur
// (1/2 = 0.5, 1/4 = 0.25, …) so the `if (emitDur > timer) return 0` early-out
// in GetEmitNum doesn't fire on rounding.

namespace
{

auto MarkInit(float color_r) {
    return [color_r](Particle& p, double /*duration*/) {
        p.color.x() = color_r;
    };
}

ParticleBoxEmitterArgs MakeBox() {
    ParticleBoxEmitterArgs a;
    a.directions    = { 1.0f, 1.0f, 1.0f };
    a.minDistance   = { 0.0f, 0.0f, 0.0f };
    a.maxDistance   = { 0.0f, 0.0f, 0.0f }; // collapse position randomness
    a.emitSpeed     = 2.0f;                 // emitDur = 0.5
    a.orgin         = { 0.0f, 0.0f, 0.0f };
    a.one_per_frame = false;
    a.sort          = false;
    a.instantaneous = 0u;
    a.minSpeed      = 0.0f;
    a.maxSpeed      = 0.0f;
    a.batchSize     = 1u;
    a.burstRate     = 0.0f;
    return a;
}

ParticleSphereEmitterArgs MakeSphere() {
    ParticleSphereEmitterArgs a;
    a.directions    = { 1.0f, 1.0f, 1.0f };
    a.minDistance   = 0.0f;
    a.maxDistance   = 0.0f;
    a.emitSpeed     = 2.0f;
    a.orgin         = { 0.0f, 0.0f, 0.0f };
    a.sign          = { 0, 0, 0 };
    a.one_per_frame = false;
    a.sort          = false;
    a.instantaneous = 0u;
    a.minSpeed      = 0.0f;
    a.maxSpeed      = 0.0f;
    a.batchSize     = 1u;
    a.burstRate     = 0.0f;
    return a;
}

} // namespace

// ============================================================================
// ParticleBoxEmitter — continuous mode
// ============================================================================

TEST_SUITE("ParticleBoxEmitter") {
    TEST_CASE("continuous mode — single emission per emitDur") {
        auto args = MakeBox();
        auto op   = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis { MarkInit(0.5f) };

        op(ps, inis, /*maxcount*/ 100u, /*timepass*/ 0.5);
        CHECK(ps.size() == 1u);
        CHECK(ps[0].color.x() == doctest::Approx(0.5f));
    }

    TEST_CASE("continuous mode — sub-emitDur tick produces nothing") {
        auto args = MakeBox();
        auto op   = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 0.25); // < emitDur
        CHECK(ps.size() == 0u);
    }

    TEST_CASE("continuous mode — accumulates timer across calls") {
        auto args = MakeBox();
        auto op   = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 0.25);
        op(ps, inis, 100u, 0.25); // total 0.5 → 1 emit
        CHECK(ps.size() == 1u);
    }

    TEST_CASE("continuous mode — large timepass emits multiple") {
        auto args = MakeBox();
        auto op   = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 2.5); // 5 emitDurs worth
        CHECK(ps.size() == 5u);
    }

    TEST_CASE("continuous mode — capped at maxcount") {
        auto args      = MakeBox();
        args.emitSpeed = 100.0f; // emitDur = 0.01 (exact-ish; many emits)
        auto op        = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, /*maxcount*/ 3u, /*timepass*/ 1.0);
        CHECK(ps.size() == 3u);
    }

    TEST_CASE("instantaneous mode — first call on empty spawns N") {
        auto args          = MakeBox();
        args.emitSpeed     = 1.0f;
        args.instantaneous = 7u;
        auto op            = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, /*maxcount*/ 100u, /*timepass*/ 0.0);
        CHECK(ps.size() == 7u);
    }

    TEST_CASE("instantaneous mode — does not retrigger when ps non-empty") {
        auto args          = MakeBox();
        args.emitSpeed     = 1.0f;
        args.instantaneous = 5u;
        auto op            = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        ps.emplace_back();
        std::vector<ParticleInitOp> inis;
        op(ps, inis, /*maxcount*/ 100u, /*timepass*/ 0.0);
        // emit_num falls back to GetEmitNum which is 0 for timepass=0.
        CHECK(ps.size() == 1u);
    }

    TEST_CASE("one_per_frame — clamps emit_num to 1") {
        auto args          = MakeBox();
        args.emitSpeed     = 100.0f;
        args.one_per_frame = true;
        auto op            = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 1.0); // would be 100 without clamp
        CHECK(ps.size() == 1u);
    }

    TEST_CASE("batchSize > 1 in continuous mode — also clamps to 1") {
        auto args      = MakeBox();
        args.emitSpeed = 100.0f;
        args.batchSize = 5u;
        auto op        = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 1.0);
        CHECK(ps.size() == 1u);
    }

    TEST_CASE("burst mode — starts exhausted, first call before burstRate emits 0") {
        auto args      = MakeBox();
        args.emitSpeed = 100.0f;
        args.batchSize = 3u;
        args.burstRate = 1.0f;
        auto op        = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 0.001);
        CHECK(ps.size() == 0u);
    }

    TEST_CASE("burst mode — after burst window resets and emits up to batchSize") {
        auto args      = MakeBox();
        args.emitSpeed = 4.0f; // emitDur = 0.25
        args.batchSize = 3u;
        args.burstRate = 10.0f; // long burst window so we drive a single one
        auto op        = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;

        // Drive past initial exhausted phase — initial burstTimer is in
        // [0, burstRate], so a single tick of `burstRate` guarantees the
        // wrap branch fires.
        op(ps, inis, 100u, args.burstRate);

        // After the wrap, ticks of emitDur=0.25 fire at most one particle per
        // call (one_per_frame clamp inside burst mode).  Drive enough ticks
        // to fill the batch.
        for (int i = 0; i < 50 && ps.size() < args.batchSize; ++i) {
            op(ps, inis, 100u, 0.25);
        }
        CHECK(ps.size() == args.batchSize);
    }

    TEST_CASE("burst mode — remaining-batch cap blocks further emits within burst") {
        auto args      = MakeBox();
        args.emitSpeed = 100.0f; // would emit many per frame without cap
        args.batchSize = 2u;
        args.burstRate = 100.0f; // ample window so no second wrap
        auto op        = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, args.burstRate); // wrap into a fresh burst
        // Within this single burst, ticks must never exceed batchSize.
        for (int i = 0; i < 30; ++i) op(ps, inis, 100u, 0.01);
        CHECK(ps.size() == args.batchSize);
    }

    TEST_CASE("sort=true — dead particle slots get reused before pushing new") {
        auto args = MakeBox();
        args.sort = true;
        auto op   = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle> ps;
        ps.resize(2);
        ps[0].lifetime = -1.0f; // dead
        ps[0].mark_new = false;
        ps[1].lifetime = 0.5f;
        ps[1].mark_new = false;
        std::vector<ParticleInitOp> inis;

        op(ps, inis, 100u, 0.5);
        REQUIRE(ps.size() == 2u); // no push, just slot reuse
        bool any_new = false;
        for (const auto& p : ps) {
            if (ParticleModify::IsNew(p)) any_new = true;
        }
        CHECK(any_new);
    }

    TEST_CASE("sort=true — pushes new particle when no dead slots") {
        auto args = MakeBox();
        args.sort = true;
        auto op   = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle> ps;
        ps.resize(1);
        ps[0].lifetime = 1.0f;
        ps[0].mark_new = false;
        std::vector<ParticleInitOp> inis;

        op(ps, inis, 100u, 0.5);
        CHECK(ps.size() == 2u);
    }

    TEST_CASE("origin offset is applied to spawned position") {
        auto args  = MakeBox();
        args.orgin = { 5.0f, -3.0f, 2.0f };
        auto op    = ParticleBoxEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 0.5);
        REQUIRE(ps.size() == 1u);
        CHECK(ps[0].position.x() == doctest::Approx(5.0f));
        CHECK(ps[0].position.y() == doctest::Approx(-3.0f));
        CHECK(ps[0].position.z() == doctest::Approx(2.0f));
    }
}

// ============================================================================
// ParticleSphereEmitter — sign mask + GenSphere
// ============================================================================

TEST_SUITE("ParticleSphereEmitter") {
    TEST_CASE("continuous mode — single emission per emitDur") {
        auto args = MakeSphere();
        auto op   = ParticleSphereEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 0.5);
        CHECK(ps.size() == 1u);
    }

    TEST_CASE("continuous mode — capped at maxcount") {
        auto args      = MakeSphere();
        args.emitSpeed = 100.0f;
        auto op        = ParticleSphereEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 4u, 1.0);
        CHECK(ps.size() == 4u);
    }

    TEST_CASE("instantaneous mode on empty") {
        auto args          = MakeSphere();
        args.instantaneous = 6u;
        auto op            = ParticleSphereEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 0.0);
        CHECK(ps.size() == 6u);
    }

    TEST_CASE("one_per_frame clamp") {
        auto args          = MakeSphere();
        args.emitSpeed     = 100.0f;
        args.one_per_frame = true;
        auto op            = ParticleSphereEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 1.0);
        CHECK(ps.size() == 1u);
    }

    TEST_CASE("batchSize > 1 in continuous mode clamps to 1") {
        auto args      = MakeSphere();
        args.emitSpeed = 100.0f;
        args.batchSize = 4u;
        auto op        = ParticleSphereEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 1.0);
        CHECK(ps.size() == 1u);
    }

    TEST_CASE("burst mode — starts exhausted") {
        auto args      = MakeSphere();
        args.emitSpeed = 100.0f;
        args.batchSize = 2u;
        args.burstRate = 1.0f;
        auto op        = ParticleSphereEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 0.001);
        CHECK(ps.size() == 0u);
    }

    TEST_CASE("burst mode — fires up to batchSize after burst window") {
        auto args      = MakeSphere();
        args.emitSpeed = 4.0f;
        args.batchSize = 3u;
        args.burstRate = 0.5f;
        auto op        = ParticleSphereEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 2.0 * args.burstRate);
        for (int i = 0; i < 50 && ps.size() < args.batchSize; ++i) {
            op(ps, inis, 100u, 0.25);
        }
        CHECK(ps.size() == args.batchSize);
    }

    TEST_CASE("ApplySign — non-zero sign forces axis sign") {
        auto args        = MakeSphere();
        args.minDistance = 1.0f;
        args.maxDistance = 1.0f;
        args.sign        = { 1, -1, 0 };
        args.directions  = { 1.0f, 1.0f, 1.0f };
        args.orgin       = { 0.0f, 0.0f, 0.0f };
        auto op          = ParticleSphereEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        // One large tick — exact emit count is irrelevant; we only check the
        // sign property of every spawned particle.
        op(ps, inis, 100u, 5.0);
        REQUIRE(ps.size() >= 1u);
        for (const auto& p : ps) {
            CHECK(p.position.x() >= 0.0f); // sign[0] = +1
            CHECK(p.position.y() <= 0.0f); // sign[1] = -1
        }
    }

    TEST_CASE("origin offset is applied to spawned position") {
        auto args  = MakeSphere();
        args.orgin = { 4.0f, 5.0f, 6.0f };
        auto op    = ParticleSphereEmitterArgs::MakeEmittOp(args);

        std::vector<Particle>       ps;
        std::vector<ParticleInitOp> inis;
        op(ps, inis, 100u, 0.5);
        REQUIRE(ps.size() == 1u);
        CHECK(ps[0].position.x() == doctest::Approx(4.0f));
        CHECK(ps[0].position.y() == doctest::Approx(5.0f));
        CHECK(ps[0].position.z() == doctest::Approx(6.0f));
    }
}
