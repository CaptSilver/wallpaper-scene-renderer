#include <doctest.h>

#include "SceneWallpaperSurface.hpp"
#include "Core/Random.hpp"
#include "Particle/ParticleEmitter.h"
#include "Particle/Particle.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace wallpaper;

// ============================================================================
// Deterministic-rendering mode (spec D11 phase-1) — CPU-only proof.
//
// This suite proves the two engine-side determinism mechanisms WITHOUT a
// viewer or a Vulkan device:
//   1. SelectFrameDt() — the pure dt-selection helper the render-thread draw
//      loop uses to feed a FIXED step instead of wall-clock when the mode is
//      on (SceneWallpaper.cpp dt chokepoint).
//   2. The thread-local particle PRNG seed — seeding effolkronium's Random
//      with a fixed value makes ParticleEmitter spawns (position / velocity /
//      per-particle random_seed) bit-identical run-to-run.  In production this
//      seed is applied on the render thread at scene load (the thread that
//      steps the emitter); here the doctest main thread plays both roles, so
//      the same thread-local invariant holds.
//
// No Vulkan, no SceneWallpaper, no GPU — only the headers already linked into
// backend_scene_tests (wpParticle + the header-only RenderInitInfo helpers).
// ============================================================================

namespace
{

// Sphere emitter whose spawned particles are RNG-driven on BOTH position and
// velocity: minDistance != maxDistance and minSpeed != maxSpeed force the
// GenSphere lambda through Random::get<normal_distribution<>>() and
// Random::get(minSpeed, maxSpeed) — the exact production RNG draws.  emitSpeed
// 2.0 gives an exactly-representable emitDur = 0.5 so a 0.5s tick emits one.
ParticleSphereEmitterArgs MakeRngSphere() {
    ParticleSphereEmitterArgs a;
    a.directions    = { 1.0f, 1.0f, 1.0f };
    a.minDistance   = 1.0f;
    a.maxDistance   = 5.0f; // != minDistance → radius is random
    a.emitSpeed     = 2.0f; // emitDur = 0.5
    a.orgin         = { 0.0f, 0.0f, 0.0f };
    a.sign          = { 0, 0, 0 };
    a.one_per_frame = false;
    a.sort          = false;
    a.instantaneous = 0u;
    a.minSpeed      = 1.0f;
    a.maxSpeed      = 10.0f; // != minSpeed → speed is random
    a.batchSize     = 1u;
    a.burstRate     = 0.0f;
    return a;
}

// A compact, bit-comparable snapshot of one particle's RNG-derived state.
struct ParticleSample {
    float    pos[3];
    float    vel[3];
    uint32_t random_seed;

    bool BitEqual(const ParticleSample& o) const {
        // memcmp on the float bytes is intentional: determinism means the SAME
        // bits, not "approximately equal".  NaN/-0.0 edge cases would also be
        // caught (they are not expected here, but the test must not paper over
        // them with an epsilon).
        return std::memcmp(pos, o.pos, sizeof(pos)) == 0 &&
               std::memcmp(vel, o.vel, sizeof(vel)) == 0 && random_seed == o.random_seed;
    }
};

// Seed the thread-local Random, then step a fresh sphere emitter `frames`
// times at a FIXED dt, recording every spawned particle's RNG-derived state.
// Reproduces the production sequence: seed-once-then-step on one thread.
std::vector<ParticleSample> RunSeededEmitter(uint32_t seed, int frames, double fixed_dt) {
    Random::seed(seed);

    auto                        op = ParticleSphereEmitterArgs::MakeEmittOp(MakeRngSphere());
    std::vector<Particle>       ps;
    std::vector<ParticleInitOp> inis; // no initializers — isolate emitter RNG

    for (int f = 0; f < frames; ++f) {
        op(ps, inis, /*maxcount*/ 1000u, fixed_dt);
    }

    std::vector<ParticleSample> out;
    out.reserve(ps.size());
    for (const auto& p : ps) {
        ParticleSample s;
        s.pos[0]      = p.position.x();
        s.pos[1]      = p.position.y();
        s.pos[2]      = p.position.z();
        s.vel[0]      = p.velocity.x();
        s.vel[1]      = p.velocity.y();
        s.vel[2]      = p.velocity.z();
        s.random_seed = p.random_seed;
        out.push_back(s);
    }
    return out;
}

bool SamplesBitEqual(const std::vector<ParticleSample>& a, const std::vector<ParticleSample>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (! a[i].BitEqual(b[i])) return false;
    }
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// SelectFrameDt — the fixed-timestep chokepoint helper
// ─────────────────────────────────────────────────────────────────────────────

TEST_SUITE("DeterminismFixedTimestep") {
    TEST_CASE("SelectFrameDt: deterministic ON returns the fixed step, ignoring wall dt") {
        const double fixed = 1.0 / 60.0;
        // Wall dt is varied across "frames"; each must collapse to `fixed`.
        CHECK(SelectFrameDt(true, fixed, 0.0) == fixed);
        CHECK(SelectFrameDt(true, fixed, 0.013) == fixed);
        CHECK(SelectFrameDt(true, fixed, 0.099) == fixed);
        CHECK(SelectFrameDt(true, fixed, 1234.5) == fixed);
    }

    TEST_CASE("SelectFrameDt: deterministic OFF passes the wall dt through unchanged") {
        const double fixed = 1.0 / 60.0;
        CHECK(SelectFrameDt(false, fixed, 0.0) == 0.0);
        CHECK(SelectFrameDt(false, fixed, 0.013) == 0.013);
        CHECK(SelectFrameDt(false, fixed, 0.099) == 0.099);
        // Off-path is the wall value verbatim — NOT the fixed step.
        CHECK(SelectFrameDt(false, fixed, 0.05) != fixed);
    }

    TEST_CASE("SelectFrameDt: a custom fixed step is honoured") {
        CHECK(SelectFrameDt(true, 1.0 / 30.0, 0.5) == doctest::Approx(1.0 / 30.0));
        CHECK(SelectFrameDt(true, 0.004, 0.5) == doctest::Approx(0.004));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ResolveDeterministic — env-var override folding
// ─────────────────────────────────────────────────────────────────────────────

TEST_SUITE("DeterminismEnvResolve") {
    TEST_CASE("ResolveDeterministic: struct flag true short-circuits true regardless of env") {
        // Struct-true must win even with the env explicitly disabling.
        ::setenv("WEK_DETERMINISTIC", "0", 1);
        CHECK(ResolveDeterministic(true) == true);
        ::unsetenv("WEK_DETERMINISTIC");
    }

    TEST_CASE("ResolveDeterministic: env unset/empty + struct false stays false") {
        ::unsetenv("WEK_DETERMINISTIC");
        CHECK(ResolveDeterministic(false) == false);
        ::setenv("WEK_DETERMINISTIC", "", 1);
        CHECK(ResolveDeterministic(false) == false);
        ::unsetenv("WEK_DETERMINISTIC");
    }

    TEST_CASE("ResolveDeterministic: env truthy enables; falsey keeps disabled") {
        ::setenv("WEK_DETERMINISTIC", "1", 1);
        CHECK(ResolveDeterministic(false) == true);
        ::setenv("WEK_DETERMINISTIC", "true", 1);
        CHECK(ResolveDeterministic(false) == true);
        ::setenv("WEK_DETERMINISTIC", "yes", 1); // any non-falsey token enables
        CHECK(ResolveDeterministic(false) == true);

        ::setenv("WEK_DETERMINISTIC", "0", 1);
        CHECK(ResolveDeterministic(false) == false);
        ::setenv("WEK_DETERMINISTIC", "false", 1);
        CHECK(ResolveDeterministic(false) == false);
        ::setenv("WEK_DETERMINISTIC", "off", 1);
        CHECK(ResolveDeterministic(false) == false);
        ::unsetenv("WEK_DETERMINISTIC");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Seedable particle RNG — the bit-identical reproducibility proof
// ─────────────────────────────────────────────────────────────────────────────

TEST_SUITE("DeterminismSeed") {
    TEST_CASE("same seed + fixed dt → two emitter runs are bit-identical") {
        const uint32_t seed     = 0x9E3779B9u;
        const double   fixed_dt = 1.0 / 60.0;
        const int      frames   = 240; // ~4s of sim at 60fps → many spawns

        auto run_a = RunSeededEmitter(seed, frames, fixed_dt);
        auto run_b = RunSeededEmitter(seed, frames, fixed_dt);

        REQUIRE(run_a.size() > 0u); // the emitter actually spawned
        REQUIRE(run_a.size() == run_b.size());
        // Sanity: at least one particle carries RNG-driven (non-origin) state,
        // so a bit-identical match is meaningful rather than two all-zero runs.
        bool any_rng_state = false;
        for (const auto& s : run_a) {
            if (s.random_seed != 0u || s.pos[0] != 0.0f || s.pos[1] != 0.0f || s.pos[2] != 0.0f) {
                any_rng_state = true;
                break;
            }
        }
        CHECK(any_rng_state);

        CHECK(SamplesBitEqual(run_a, run_b));
    }

    TEST_CASE("reproducibility holds across more than two runs") {
        const uint32_t seed     = 12345u;
        const double   fixed_dt = 1.0 / 120.0;
        const int      frames   = 120;

        auto baseline = RunSeededEmitter(seed, frames, fixed_dt);
        REQUIRE(baseline.size() > 0u);
        for (int rep = 0; rep < 4; ++rep) {
            auto again = RunSeededEmitter(seed, frames, fixed_dt);
            CHECK(SamplesBitEqual(baseline, again));
        }
    }

    TEST_CASE("it is the SEED that makes runs match — a different seed diverges") {
        const double fixed_dt = 1.0 / 60.0;
        const int    frames   = 240;

        auto run_seed_a = RunSeededEmitter(0xAAAAAAAAu, frames, fixed_dt);
        auto run_seed_b = RunSeededEmitter(0x55555555u, frames, fixed_dt);

        REQUIRE(run_seed_a.size() > 0u);
        REQUIRE(run_seed_b.size() > 0u);
        // Negative control: distinct seeds must NOT produce the same stream.
        // (Same emit COUNT is fine — emission cadence is dt-driven, not RNG —
        // but the per-particle RNG state must differ.)
        CHECK_FALSE(SamplesBitEqual(run_seed_a, run_seed_b));
    }

    TEST_CASE("per-particle random_seed is itself reproducible under a fixed global seed") {
        // ParticleEmitter assigns particle.random_seed = Random::get<uint32_t>(...)
        // at spawn; under a fixed global seed that per-particle stream must be
        // stable, since downstream operators (remapvalue input:random) hash it.
        const uint32_t seed     = 777u;
        const double   fixed_dt = 1.0 / 60.0;
        const int      frames   = 60;

        auto a = RunSeededEmitter(seed, frames, fixed_dt);
        auto b = RunSeededEmitter(seed, frames, fixed_dt);
        REQUIRE(a.size() == b.size());
        REQUIRE(a.size() > 0u);
        for (size_t i = 0; i < a.size(); ++i) {
            CHECK(a[i].random_seed == b[i].random_seed);
            CHECK(a[i].random_seed != 0u); // emitter lower-bounds the seed to 1
        }
    }
}
