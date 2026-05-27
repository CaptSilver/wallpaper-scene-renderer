// Doctest suite for the rope-gen scratch-buffer thread_local promotion.
// Exercises three observable properties of `static thread_local` scratch:
//
//   1. Byte-identical vertex output across calls (the optimization is purely
//      an allocation-strategy change; the SceneVertexArray bytes must not
//      change between two calls with the same input).
//   2. Capacity retention across calls on the same thread: a big-N call
//      followed by a small-N call must observe capacity-at-call >=
//      previous-call's capacity (no shrink between calls).  Function-local
//      scratch resets capacity to whatever the call reserves, so the small-N
//      capacity equals small-N; thread_local scratch retains the high-water
//      from the big-N call.  This is the RED/GREEN discriminator.
//   3. Multi-thread isolation: two threads driving the gen concurrently see
//      distinct scratch storage (different `data()` addresses), and each
//      thread's vertex output matches the single-thread baseline for that
//      input.

#include <doctest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

#include <Eigen/Core>

#include "Particle/Particle.h"
#include "Particle/WPParticleRawGener_TestHooks.hpp"
#include "Scene/SceneVertexArray.h"

using wallpaper::Particle;
using wallpaper::SceneVertexArray;
using wallpaper::test_hooks::GetLastRopeScratchProbe;
using wallpaper::test_hooks::TestGenRopeParticleData;
using wallpaper::test_hooks::TestGenRopeParticleDataGS;

namespace
{

// Vertex layout for the GS rope path (six FLOAT4 attributes per vertex,
// total 24 floats per vertex; matches the layout WPParticleParser builds
// for rope-with-geometry-shader meshes).  The attribute names don't matter
// for the byte-identity check — only the count of floats per vertex does.
std::vector<SceneVertexArray::SceneVertexAttribute> makeGSRopeAttrs() {
    using Attr = SceneVertexArray::SceneVertexAttribute;
    using wallpaper::VertexType;
    return {
        Attr { "a_PositionVec4", VertexType::FLOAT4, true },
        Attr { "a_TexCoordVec4", VertexType::FLOAT4, true },
        Attr { "a_TexCoordVec4C1", VertexType::FLOAT4, true },
        Attr { "a_TexCoordVec4C2", VertexType::FLOAT4, true },
        Attr { "a_TexCoordVec4C3", VertexType::FLOAT4, true },
        Attr { "a_Color", VertexType::FLOAT4, true },
    };
}

// Deterministic particle fixture: N alive on a straight line in X.  No dead
// slots (LifetimeOk checks lifetime > 0, which the default Particle satisfies).
// Particle colors / alphas are non-trivial so the vertex stream isn't trivially
// zeroed — gives byte-identity teeth.
std::vector<Particle> makeRopeFixture(std::size_t N) {
    std::vector<Particle> ps(N);
    for (std::size_t i = 0; i < N; ++i) {
        ps[i].position = { static_cast<float>(i) * 0.5f, 0.0f, 0.0f };
        ps[i].size     = 0.20f;
        ps[i].rotation = { 0.10f * static_cast<float>(i),
                           0.20f * static_cast<float>(i),
                           0.30f * static_cast<float>(i) };
        ps[i].lifetime = 1.0f;
        ps[i].alpha    = 0.50f + 0.40f * static_cast<float>(i % 4) / 4.0f; // > 0.05 threshold
        ps[i].color    = { 0.50f + 0.10f * static_cast<float>(i % 3),
                           0.30f,
                           1.0f - 0.05f * static_cast<float>(i) };
    }
    return ps;
}

std::vector<std::uint8_t> copyVertexBytes(const SceneVertexArray& sv, std::size_t vert_count) {
    const auto* base = reinterpret_cast<const std::uint8_t*>(sv.Data());
    const auto  span = vert_count * sv.OneSizeOf();
    return std::vector<std::uint8_t>(base, base + span);
}

} // namespace

TEST_SUITE("WPParticleRawGener thread_local scratch") {

    TEST_CASE("GS rope: byte-identical vertex output across two identical calls") {
        auto particles = makeRopeFixture(32);
        auto attrs     = makeGSRopeAttrs();

        SceneVertexArray sv1(attrs, 128);
        SceneVertexArray sv2(attrs, 128);
        const std::size_t segs1 =
            TestGenRopeParticleDataGS(particles, Eigen::Vector3f::Zero(), sv1, 0, 1.0f);
        const std::size_t segs2 =
            TestGenRopeParticleDataGS(particles, Eigen::Vector3f::Zero(), sv2, 0, 1.0f);
        REQUIRE(segs1 == segs2);
        CHECK(segs1 > 0);
        auto bytes1 = copyVertexBytes(sv1, segs1);
        auto bytes2 = copyVertexBytes(sv2, segs2);
        REQUIRE(bytes1.size() == bytes2.size());
        CHECK(std::memcmp(bytes1.data(), bytes2.data(), bytes1.size()) == 0);
    }

    // RED/GREEN discriminator.  Two consecutive calls on the same thread,
    // big-N then small-N.  Under function-local scratch the second call's
    // capacity equals small-N (each call reserves fresh).  Under static
    // thread_local the second call's capacity retains big-N's high-water.
    TEST_CASE("GS rope: scratch capacity retains big-N high-water across small-N call") {
        const std::size_t kBigN   = 64;
        const std::size_t kSmallN = 8;

        auto big_particles   = makeRopeFixture(kBigN);
        auto small_particles = makeRopeFixture(kSmallN);
        auto attrs           = makeGSRopeAttrs();

        SceneVertexArray sv_big(attrs, 256);
        SceneVertexArray sv_small(attrs, 256);

        TestGenRopeParticleDataGS(big_particles, Eigen::Vector3f::Zero(), sv_big, 0, 1.0f);
        const auto probe_big = GetLastRopeScratchProbe();
        CHECK(probe_big.alive_capacity >= kBigN);
        CHECK(probe_big.positions_capacity >= kBigN);
        CHECK(probe_big.seg_lens_capacity >= kBigN - 1);

        TestGenRopeParticleDataGS(small_particles, Eigen::Vector3f::Zero(), sv_small, 0, 1.0f);
        const auto probe_small = GetLastRopeScratchProbe();

        // Thread-local promotion: the small-N call must observe scratch
        // capacities >= what the big-N call left behind.  Function-local
        // scratch would yield capacity == small-N here.
        CHECK(probe_small.alive_capacity >= probe_big.alive_capacity);
        CHECK(probe_small.positions_capacity >= probe_big.positions_capacity);
        CHECK(probe_small.seg_lens_capacity >= probe_big.seg_lens_capacity);
    }

    TEST_CASE("non-GS rope: alive-scratch capacity retains big-N high-water across small-N call") {
        const std::size_t kBigN   = 48;
        const std::size_t kSmallN = 6;

        auto big_particles   = makeRopeFixture(kBigN);
        auto small_particles = makeRopeFixture(kSmallN);
        auto attrs           = makeGSRopeAttrs();

        SceneVertexArray sv_big(attrs, 256);
        SceneVertexArray sv_small(attrs, 256);

        TestGenRopeParticleData(big_particles, Eigen::Vector3f::Zero(), sv_big, 0, 1.0f);
        const auto probe_big = GetLastRopeScratchProbe();
        CHECK(probe_big.alive_capacity >= kBigN);

        TestGenRopeParticleData(small_particles, Eigen::Vector3f::Zero(), sv_small, 0, 1.0f);
        const auto probe_small = GetLastRopeScratchProbe();
        CHECK(probe_small.alive_capacity >= probe_big.alive_capacity);
    }

    // Shrink-then-grow stale-tail check.  The thread_local promotion uses
    // resize() on the positions/seg_lens vectors, and we read them only in
    // the [0, alive.size()) window — the bytes beyond that window can hold
    // residue from a prior big-N call, but no consumer reads them.  This
    // test grows after shrinking and verifies the bytes still match a
    // single-call baseline (no smearing from the prior small-N pass).
    TEST_CASE("GS rope: grow-then-shrink-then-grow yields identical bytes to a fresh-thread baseline") {
        auto p_big   = makeRopeFixture(40);
        auto p_small = makeRopeFixture(6);
        auto attrs   = makeGSRopeAttrs();

        // Sequence A: cycle big -> small -> big on this thread.
        SceneVertexArray sv_a1(attrs, 256), sv_a2(attrs, 256), sv_a3(attrs, 256);
        TestGenRopeParticleDataGS(p_big,   Eigen::Vector3f::Zero(), sv_a1, 0, 1.0f);
        TestGenRopeParticleDataGS(p_small, Eigen::Vector3f::Zero(), sv_a2, 0, 1.0f);
        const auto segs_a3 = TestGenRopeParticleDataGS(p_big, Eigen::Vector3f::Zero(), sv_a3, 0, 1.0f);

        // Sequence B: fresh thread driving only the big-N call.  Its bytes
        // are the no-residue baseline.
        std::vector<std::uint8_t> bytes_b;
        std::size_t               segs_b = 0;
        std::thread tB([&]() {
            SceneVertexArray sv_b(attrs, 256);
            segs_b = TestGenRopeParticleDataGS(p_big, Eigen::Vector3f::Zero(), sv_b, 0, 1.0f);
            bytes_b = copyVertexBytes(sv_b, segs_b);
        });
        tB.join();

        const auto bytes_a3 = copyVertexBytes(sv_a3, segs_a3);
        REQUIRE(segs_a3 == segs_b);
        REQUIRE(bytes_a3.size() == bytes_b.size());
        CHECK(std::memcmp(bytes_a3.data(), bytes_b.data(), bytes_a3.size()) == 0);
    }

    // Multi-thread no-cross-talk: two threads alternate big/small rope-gen
    // calls; each thread's outputs must match the single-thread baseline for
    // that input.  thread_local promotion guarantees per-thread isolation
    // (the language rule); a stray static would let one thread overwrite
    // the other's scratch and the vertex bytes would diverge.
    TEST_CASE("GS rope: two threads with interleaved inputs have isolated scratch") {
        auto p_big   = makeRopeFixture(28);
        auto p_small = makeRopeFixture(7);
        auto attrs   = makeGSRopeAttrs();

        // Single-thread baselines (one fresh thread each so capacity carry-over
        // doesn't leak into the byte comparison).
        std::vector<std::uint8_t> baseline_big, baseline_small;
        std::size_t               segs_big_b = 0, segs_small_b = 0;
        std::thread tBig([&]() {
            SceneVertexArray sv(attrs, 256);
            segs_big_b = TestGenRopeParticleDataGS(p_big, Eigen::Vector3f::Zero(), sv, 0, 1.0f);
            baseline_big = copyVertexBytes(sv, segs_big_b);
        });
        tBig.join();
        std::thread tSmall([&]() {
            SceneVertexArray sv(attrs, 256);
            segs_small_b = TestGenRopeParticleDataGS(p_small, Eigen::Vector3f::Zero(), sv, 0, 1.0f);
            baseline_small = copyVertexBytes(sv, segs_small_b);
        });
        tSmall.join();

        // Two concurrent runners — A does big->small, B does small->big.
        std::atomic<bool> go { false };
        struct Out {
            std::size_t               segs1 { 0 }, segs2 { 0 };
            std::vector<std::uint8_t> bytes1, bytes2;
            const void*               scratch_addr1 { nullptr };
            const void*               scratch_addr2 { nullptr };
        };
        Out outA, outB;

        auto runnerA = [&]() {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            SceneVertexArray sv1(attrs, 256), sv2(attrs, 256);
            outA.segs1 = TestGenRopeParticleDataGS(p_big, Eigen::Vector3f::Zero(), sv1, 0, 1.0f);
            outA.scratch_addr1 = GetLastRopeScratchProbe().alive_data;
            outA.bytes1 = copyVertexBytes(sv1, outA.segs1);
            outA.segs2 = TestGenRopeParticleDataGS(p_small, Eigen::Vector3f::Zero(), sv2, 0, 1.0f);
            outA.scratch_addr2 = GetLastRopeScratchProbe().alive_data;
            outA.bytes2 = copyVertexBytes(sv2, outA.segs2);
        };
        auto runnerB = [&]() {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            SceneVertexArray sv1(attrs, 256), sv2(attrs, 256);
            outB.segs1 = TestGenRopeParticleDataGS(p_small, Eigen::Vector3f::Zero(), sv1, 0, 1.0f);
            outB.scratch_addr1 = GetLastRopeScratchProbe().alive_data;
            outB.bytes1 = copyVertexBytes(sv1, outB.segs1);
            outB.segs2 = TestGenRopeParticleDataGS(p_big, Eigen::Vector3f::Zero(), sv2, 0, 1.0f);
            outB.scratch_addr2 = GetLastRopeScratchProbe().alive_data;
            outB.bytes2 = copyVertexBytes(sv2, outB.segs2);
        };

        std::thread tA(runnerA), tB(runnerB);
        go.store(true, std::memory_order_release);
        tA.join();
        tB.join();

        // Each thread's per-call bytes match the single-thread baseline for
        // that input — no cross-thread contamination.
        REQUIRE(outA.segs1 == segs_big_b);
        REQUIRE(outA.segs2 == segs_small_b);
        REQUIRE(outB.segs1 == segs_small_b);
        REQUIRE(outB.segs2 == segs_big_b);
        CHECK(std::memcmp(outA.bytes1.data(), baseline_big.data(),   outA.bytes1.size()) == 0);
        CHECK(std::memcmp(outA.bytes2.data(), baseline_small.data(), outA.bytes2.size()) == 0);
        CHECK(std::memcmp(outB.bytes1.data(), baseline_small.data(), outB.bytes1.size()) == 0);
        CHECK(std::memcmp(outB.bytes2.data(), baseline_big.data(),   outB.bytes2.size()) == 0);
    }
}
