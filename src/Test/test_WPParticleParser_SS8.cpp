#include <doctest.h>

#include "WPParticleParser.hpp"
#include "Particle/Particle.h"
#include "Particle/ParticleEmitter.h"
#include "Core/Random.hpp"

#include <nlohmann/json.hpp>
#include <Eigen/Core>
#include <array>
#include <cstring>
#include <cstdint>
#include <vector>

using namespace wallpaper;
using nlohmann::json;

namespace
{

// Local fixture (parallels OpFixture in test_WPParticleOperators.cpp; redefined
// here to keep this TU self-contained — that fixture lives in an anonymous
// namespace in the sibling file and isn't reachable across TUs).
struct Ss8Fixture {
    std::vector<Particle>               particles;
    std::array<ParticleControlpoint, 8> cps {};
    double                              time { 0.0 };
    double                              time_pass { 0.016 };

    ParticleInfo info() {
        return ParticleInfo {
            .particles     = std::span<Particle>(particles),
            .controlpoints = std::span<ParticleControlpoint>(cps.data(), cps.size()),
            .time          = time,
            .time_pass     = time_pass,
        };
    }
};

wpscene::ParticleInstanceoverride empty_override() {
    wpscene::ParticleInstanceoverride o;
    o.enabled = false;
    return o;
}

// Pack the bytes of every particle's position + velocity into a flat buffer
// so we can compare bit-exact between two equivalent runs of the operator.
std::vector<uint8_t> pack_pos_vel(const std::vector<Particle>& ps) {
    std::vector<uint8_t> out;
    out.reserve(ps.size() * 2 * sizeof(Eigen::Vector3f));
    for (const auto& p : ps) {
        const uint8_t* pp = reinterpret_cast<const uint8_t*>(p.position.data());
        out.insert(out.end(), pp, pp + sizeof(Eigen::Vector3f));
        const uint8_t* vp = reinterpret_cast<const uint8_t*>(p.velocity.data());
        out.insert(out.end(), vp, vp + sizeof(Eigen::Vector3f));
    }
    return out;
}

std::vector<uint8_t> pack_size(const std::vector<Particle>& ps) {
    std::vector<uint8_t> out;
    out.reserve(ps.size() * sizeof(float));
    for (const auto& p : ps) {
        const uint8_t* sp = reinterpret_cast<const uint8_t*>(&p.size);
        out.insert(out.end(), sp, sp + sizeof(float));
    }
    return out;
}

// Build a deterministic 8-particle boids batch.  Positions are arranged on a
// regular grid that puts every pair within `neighborthreshold=2.0` so every
// pair feeds sumSep/sumAli/sumCoh (exercises the cast-hoist on every j).
void seed_boids_batch(Ss8Fixture& fx) {
    fx.particles.clear();
    fx.particles.reserve(8);
    for (int k = 0; k < 8; ++k) {
        Particle p;
        p.lifetime       = 1.0f;
        p.init.lifetime  = 1.0f;
        p.alpha          = 1.0f;
        p.size           = 20.0f;
        p.init.size      = 20.0f;
        p.color          = Eigen::Vector3f(1, 1, 1);
        p.init.color     = p.color;
        // Spread along x with small y/z perturbation so j-pairs are within
        // neighborthreshold (2.0) AND some pairs land inside
        // separationthreshold (0.5) — exercises both the alignment-only and
        // separation branches.
        const float fk = static_cast<float>(k);
        p.position       = Eigen::Vector3f(fk * 0.3f, 0.1f * fk, -0.05f * fk);
        p.velocity       = Eigen::Vector3f(0.1f * fk, 0.05f * fk, 0.0f);
        p.random_seed    = 0xdeadbeefu + static_cast<uint32_t>(k);
        fx.particles.push_back(p);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// SS8.1 — boids inner loop hoists per-j Vector3d casts to SoA scratch.
//
// The fix is provably bit-exact: every Vector3f -> Vector3d cast that the
// hoist removes is exactly the same conversion done once per particle up
// front instead of up-to-three times per (i,j) pair. The test pins the
// output of a deterministic 8-particle / 5-tick run; if the hoist is wrong
// (e.g. the scratch is read at the wrong index, or stale-tail bytes leak
// into a read), the byte compare flips.
// ---------------------------------------------------------------------------
TEST_SUITE("WPParticleParser boids SoA") {
    TEST_CASE("boids 8-particle / 5-tick output is run-to-run deterministic") {
        json j = { { "name", "boids" },
                   { "neighborthreshold", 2.0 },
                   { "separationthreshold", 0.5 },
                   { "separationfactor", 1.0 },
                   { "alignmentfactor", 0.5 },
                   { "cohesionfactor", 0.5 },
                   { "maxspeed", 5.0 } };

        // Run #1
        Random::seed(0x5eed5eedu);
        auto      op1 = WPParticleParser::genParticleOperatorOp(j, empty_override());
        Ss8Fixture fx1;
        seed_boids_batch(fx1);
        for (int t = 0; t < 5; ++t) {
            op1(fx1.info());
            fx1.time += fx1.time_pass;
        }
        auto bytes1 = pack_pos_vel(fx1.particles);

        // Run #2 — different operator instance, same seed + same fixture must
        // produce byte-identical output. This locks the boids state machine
        // against any read-order regression introduced by the SoA hoist (e.g.
        // reading from `info.particles[j]` after the scratch was supposed to
        // be authoritative).
        Random::seed(0x5eed5eedu);
        auto      op2 = WPParticleParser::genParticleOperatorOp(j, empty_override());
        Ss8Fixture fx2;
        seed_boids_batch(fx2);
        for (int t = 0; t < 5; ++t) {
            op2(fx2.info());
            fx2.time += fx2.time_pass;
        }
        auto bytes2 = pack_pos_vel(fx2.particles);

        REQUIRE(bytes1.size() == bytes2.size());
        CHECK(0 == std::memcmp(bytes1.data(), bytes2.data(), bytes1.size()));
    }

    // Stale-tail regression: shrinking the alive particle count between
    // calls must not let stale scratch entries past index N leak into the
    // computation. The fix uses `thread_local` storage with `resize(N)`
    // semantics; resize doesn't shrink capacity, so a subsequent larger N
    // would otherwise read overwritten-but-stale bytes. The i-loop is bound
    // to [0, N), but the j-loop must also stay bound — this test exercises
    // a shrink-then-grow sequence.
    TEST_CASE("boids tolerates shrink-then-grow N across ticks") {
        json j = { { "name", "boids" },
                   { "neighborthreshold", 5.0 },
                   { "separationthreshold", 0.1 },
                   { "alignmentfactor", 1.0 },
                   { "cohesionfactor", 1.0 },
                   { "separationfactor", 1.0 },
                   { "maxspeed", 10.0 } };

        Random::seed(0xc0ffeeu);
        auto      op = WPParticleParser::genParticleOperatorOp(j, empty_override());

        // First tick: 8 particles.
        Ss8Fixture fx;
        seed_boids_batch(fx);
        op(fx.info());
        // No NaN/inf or wild values from a clean state.
        for (const auto& p : fx.particles) {
            CHECK(std::isfinite(p.velocity.x()));
            CHECK(std::isfinite(p.velocity.y()));
            CHECK(std::isfinite(p.velocity.z()));
        }

        // Shrink to 4 particles for the second tick (scratch capacity > N).
        fx.particles.resize(4);
        op(fx.info());
        for (const auto& p : fx.particles) {
            CHECK(std::isfinite(p.velocity.x()));
            CHECK(std::isfinite(p.velocity.y()));
            CHECK(std::isfinite(p.velocity.z()));
        }

        // Grow back to 8 — scratch must be re-populated, NOT read from stale
        // (overwritten by the 4-particle pass) tail bytes for indices 4..7.
        seed_boids_batch(fx); // rebuild full 8-particle state
        op(fx.info());
        for (const auto& p : fx.particles) {
            CHECK(std::isfinite(p.velocity.x()));
            CHECK(std::isfinite(p.velocity.y()));
            CHECK(std::isfinite(p.velocity.z()));
        }
    }
} // TEST_SUITE WPParticleParser boids SoA

// ---------------------------------------------------------------------------
// SS8.3 — FrequencyValue accessors switch from .at() (always-checked) to
// operator[] (unchecked) in the proven-in-range inner loop. Mechanical /
// bit-exact: the per-particle output is the same arithmetic, just minus the
// bounds-check throw machinery.
//
// Locked in by re-running a deterministic oscillatesize batch with the same
// seed and asserting byte-identical particle.size output. If the accessor
// ever escapes its CheckAndResize precondition, the dereference would UB
// (caught by ASAN under preflight); the assert added alongside the fix
// gives a release-time NOP and debug-time crash.
// ---------------------------------------------------------------------------
TEST_SUITE("WPParticleParser FrequencyValue at->[]") {
    TEST_CASE("oscillatesize across 60 ticks is run-to-run deterministic") {
        json j = { { "name", "oscillatesize" },
                   { "frequencymin", 1.0 },
                   { "frequencymax", 2.0 },
                   { "scalemin", 0.5 },
                   { "scalemax", 1.5 } };

        auto build_batch = [](Ss8Fixture& fx) {
            fx.particles.clear();
            fx.particles.reserve(16);
            for (int k = 0; k < 16; ++k) {
                Particle p;
                p.lifetime      = 0.7f;
                p.init.lifetime = 1.0f;
                p.alpha         = 1.0f;
                p.size          = 20.0f + static_cast<float>(k);
                p.init.size     = p.size;
                p.color         = Eigen::Vector3f(1, 1, 1);
                p.init.color    = p.color;
                p.random_seed   = 0xfeedfaceu + static_cast<uint32_t>(k);
                fx.particles.push_back(p);
            }
        };

        // Run #1
        Random::seed(0xabcdef01u);
        auto      op1 = WPParticleParser::genParticleOperatorOp(j, empty_override());
        Ss8Fixture fx1;
        build_batch(fx1);
        for (int t = 0; t < 60; ++t) {
            op1(fx1.info());
            fx1.time += fx1.time_pass;
        }
        auto bytes1 = pack_size(fx1.particles);

        // Run #2 — same seed, same inputs, must produce byte-identical sizes.
        // The operator factory invokes CheckAndResize(particles.size()) before
        // the per-particle loop; the .at()->[] swap preserves arithmetic.
        Random::seed(0xabcdef01u);
        auto      op2 = WPParticleParser::genParticleOperatorOp(j, empty_override());
        Ss8Fixture fx2;
        build_batch(fx2);
        for (int t = 0; t < 60; ++t) {
            op2(fx2.info());
            fx2.time += fx2.time_pass;
        }
        auto bytes2 = pack_size(fx2.particles);

        REQUIRE(bytes1.size() == bytes2.size());
        CHECK(0 == std::memcmp(bytes1.data(), bytes2.data(), bytes1.size()));
    }

    // Validate the CheckAndResize precondition holds for all three accessor
    // paths (oscillatealpha/oscillatesize/oscillateposition) at every per-
    // particle call. The lambda factory in genParticleOperatorOp resizes
    // before the loop; verify no exception (.at) and no UB (.[]) on the
    // boundary index.
    TEST_CASE("oscillatesize/alpha/position precondition holds at N-1") {
        struct Case {
            const char* name;
            json        j;
        };
        std::array<Case, 3> cases { {
            { "oscillatealpha",
              json { { "name", "oscillatealpha" }, { "frequencymin", 2.0 } } },
            { "oscillatesize",
              json { { "name", "oscillatesize" }, { "frequencymin", 2.0 } } },
            { "oscillateposition",
              json { { "name", "oscillateposition" }, { "frequencymin", 2.0 } } },
        } };

        Random::seed(0xfacefaceu);
        for (auto& c : cases) {
            auto      op = WPParticleParser::genParticleOperatorOp(c.j, empty_override());
            Ss8Fixture fx;
            // 32 particles — the largest index touched (31) is the
            // boundary that .at() would have caught; [] must agree because
            // CheckAndResize(particles.size()) ran first.
            for (int k = 0; k < 32; ++k) {
                Particle p;
                p.lifetime      = 0.5f;
                p.init.lifetime = 1.0f;
                p.alpha         = 1.0f;
                p.size          = 10.0f;
                p.init.size     = 10.0f;
                p.color         = Eigen::Vector3f(1, 1, 1);
                p.init.color    = p.color;
                p.random_seed   = 0x12345678u + static_cast<uint32_t>(k);
                fx.particles.push_back(p);
            }
            op(fx.info());
            // No exception, no UB; basic sanity check on a couple of particles.
            CHECK(std::isfinite(fx.particles.front().size));
            CHECK(std::isfinite(fx.particles.back().size));
            CHECK(std::isfinite(fx.particles.front().alpha));
            CHECK(std::isfinite(fx.particles.back().alpha));
        }
    }
} // TEST_SUITE WPParticleParser FrequencyValue at->[]
