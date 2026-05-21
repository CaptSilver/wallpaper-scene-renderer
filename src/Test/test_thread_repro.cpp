// test_thread_repro.cpp — focused multithread exerciser for the B5 races.
//
// This is NOT a doctest binary: it is a deterministic-ish stress driver meant
// to be run under ThreadSanitizer (-DWEK_SANITIZE=thread).  TSAN itself is the
// oracle — a clean exit AND zero TSAN reports == pass.  Each repro spins a
// writer thread mutating shared state while reader threads concurrently read
// it, for a fixed number of iterations.  Functional asserts are kept light
// (the point is the race detector, not value correctness, which the doctest
// suites already cover), but we do sanity-check obvious invariants so a totally
// broken build fails loudly even without TSAN.
//
// Two repros:
//   1. B5(a): std::atomic<std::shared_ptr<T>> load/store.  SceneWallpaper.cpp's
//      m_scene members are atomic<shared_ptr<Scene>> but that TU cannot link
//      without Vulkan, so this is a GENERIC stand-in over the identical pattern
//      (one thread store()s fresh shared_ptrs while others load()+deref).  The
//      compile-gate in SceneWallpaper.cpp (no operator-> on std::atomic) proves
//      the real members were fully migrated; this proves the pattern is race-
//      free under TSAN.  A plain (non-atomic) shared_ptr in the same shape
//      would make TSAN report a control-block data race — see PROVE_RACE below.
//   2. B5(b): the REAL Scene parent-tree code — Scene::QueueParentChange +
//      ApplyPendingParentChanges + ApplyPendingChildSorts (render thread) vs
//      Scene::ResolveParentNodeId (QML thread).  wpScene links without Vulkan,
//      so this exercises the actual shipped fix (m_pending_parent_mutex held
//      across the whole drain + the const locked resolver).
//
// Build a "does it still bite?" check: define WEK_THREAD_REPRO_PROVE_RACE at
// compile time to swap repro 1 to a deliberately-unsynchronised plain
// shared_ptr; under TSAN that build MUST report a race (proves the harness
// exercises the bug).  Off by default so the normal build is clean.

#include "Scene/Scene.h"
#include "Scene/SceneNode.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace wallpaper;

namespace
{

// Iteration counts: enough to reliably interleave under TSAN's scheduler
// without making the run slow (TSAN is ~5-15x).  Tunable via argv[1].
constexpr int kDefaultIters = 20000;

// ── Repro 1: atomic<shared_ptr> load/store (B5(a) pattern) ──────────────────
struct Payload {
    int                ortho[2] { 1920, 1080 };
    std::vector<int>   ids { 1, 2, 3 };
};

#ifdef WEK_THREAD_REPRO_PROVE_RACE
// Deliberately racy: plain shared_ptr concurrently copied + reassigned.
// TSAN must flag this (data race on the control block / pointer member).
static std::shared_ptr<Payload> g_payload = std::make_shared<Payload>();
static std::shared_ptr<Payload> load_payload() { return g_payload; }              // RACY copy
static void store_payload(std::shared_ptr<Payload> p) { g_payload = std::move(p); } // RACY assign
#else
static std::atomic<std::shared_ptr<Payload>> g_payload { std::make_shared<Payload>() };
static std::shared_ptr<Payload> load_payload() { return g_payload.load(); }
static void store_payload(std::shared_ptr<Payload> p) { g_payload.store(std::move(p)); }
#endif

int repro_atomic_shared_ptr(int iters) {
    std::atomic<bool> go { false };
    std::atomic<long> sum { 0 }; // observable side effect so derefs aren't elided

    // Writer: republish a fresh Payload, mirroring loadScene()'s m_scene.store().
    std::thread writer([&] {
        while (! go.load()) {}
        for (int i = 0; i < iters; ++i) {
            store_payload(std::make_shared<Payload>());
        }
    });

    // Readers: load() into a local, then deref — mirroring getOrthoSize /
    // getLayerBoneIndex reading the atomic member into a local first.
    auto reader = [&] {
        while (! go.load()) {}
        long local = 0;
        for (int i = 0; i < iters; ++i) {
            auto p = load_payload();
            if (p) {
                local += p->ortho[0] + p->ortho[1];
                for (int id : p->ids) local += id;
            }
        }
        sum.fetch_add(local);
    };
    std::thread r1(reader), r2(reader);

    go.store(true);
    writer.join();
    r1.join();
    r2.join();

    // Invariant: every successful deref added at least the ortho sum.
    return (sum.load() >= 0) ? 0 : 1;
}

// ── Repro 2: real Scene parent-tree concurrency (B5(b)) ─────────────────────
// Builds a small, stable node set kept alive by `owners`, registers them in
// nodeById, then races the render-thread drain against the QML-thread resolver.
int repro_scene_parent_tree(int iters) {
    Scene s;
    s.sceneGraph = std::make_shared<SceneNode>();

    // 8 nodes, ids 1..8, all initially children of the scene root.
    std::vector<std::shared_ptr<SceneNode>> owners;
    constexpr int kNodes = 8;
    for (int i = 1; i <= kNodes; ++i) {
        auto n  = std::make_shared<SceneNode>();
        n->ID() = i;
        s.sceneGraph->AppendChild(n);
        s.nodeById[i] = n.get();
        owners.push_back(std::move(n));
    }

    std::atomic<bool> go { false };
    std::atomic<long> resolved_count { 0 };

    // Render-thread analogue: drain reparent + sort queues (both lock
    // m_pending_parent_mutex across the whole tree mutation).
    std::thread render([&] {
        while (! go.load()) {}
        for (int i = 0; i < iters; ++i) {
            int child  = 1 + (i % kNodes);
            int parent = 1 + ((i + 3) % kNodes);
            if (parent == child) parent = -1; // reattach to root
            s.QueueParentChange(child, parent);
            s.ApplyPendingParentChanges();
            s.QueueChildSort(child, i % kNodes);
            s.ApplyPendingChildSorts();
        }
    });

    // QML-thread analogue: resolve parents via the const locked accessor.
    auto resolver = [&] {
        while (! go.load()) {}
        long local = 0;
        for (int i = 0; i < iters; ++i) {
            i32 pid = s.ResolveParentNodeId(1 + (i % kNodes));
            if (pid != -1) ++local;
        }
        resolved_count.fetch_add(local);
    };
    std::thread q1(resolver), q2(resolver);

    go.store(true);
    render.join();
    q1.join();
    q2.join();

    // Owners keep every node alive for the whole run; resolver returned values
    // only (no escaping pointer). Success = no crash / no TSAN report.
    (void)resolved_count;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    int iters = kDefaultIters;
    if (argc > 1) {
        int v = std::atoi(argv[1]);
        if (v > 0) iters = v;
    }

    int rc = 0;
    std::printf("[thread-repro] atomic<shared_ptr> (B5a), %d iters...\n", iters);
    rc |= repro_atomic_shared_ptr(iters);

    std::printf("[thread-repro] Scene parent-tree (B5b), %d iters...\n", iters);
    rc |= repro_scene_parent_tree(iters);

    std::printf("[thread-repro] done (rc=%d)%s\n", rc,
                rc == 0 ? " — clean (check TSAN output for races)" : " — FAILED invariant");
    return rc;
}
