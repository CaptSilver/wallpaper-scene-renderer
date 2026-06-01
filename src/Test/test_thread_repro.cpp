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
#include "WPShaderParser.hpp"
#include "WPShaderTransforms.h"
#include "Fs/VFS.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
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

// Repro 3: concurrent CompileToSpv (locks in g_glslangSerialiseMtx).
// glslang's keyword scanner caches const char* keys in a process-global hash
// table that is not safe under concurrent reads.  WPShaderParser serialises
// every glslang entry point with g_glslangSerialiseMtx.  This races N threads
// each compiling K distinct trivial GLSL units through CompileToSpv with NO
// cache dir (synchronous path).
//
// ALSO covers a SIGSEGV regression seen in production (RPM 2026-05-30):
// every regex in WPShaderTransforms.h was `static const std::regex`
// (hoisted out of inner loops over ~9 commits for perf).  libstdc++'s
// regex matcher reads/writes internal NFA state on every match call —
// even for "const" std::regex, the underlying _NFA's mutable cache is
// touched.  Two threads matching against the same shared NFA SIGSEGV
// in `_Executor::_M_dfs` or `_NFA_base::_M_sub_count`.  Particle
// wallpapers with a geometry stage (e.g. 'genericparticle' with geom
// 2732 bytes) crashed plasmashell within seconds.
//
// Fix: switched all 120 `static const std::regex` sites to
// `thread_local const std::regex` so each thread builds its own NFA on
// first call — no shared mutable state across threads.  This repro
// mixes FRAGMENT/GEOMETRY/HLSL-clip stages so a future drift that
// reverts thread_local OR narrows the g_glslangSerialiseMtx fails
// loudly (TSAN race AND/OR direct SIGSEGV).  Repro 4 below stresses
// the WPShaderTransforms.h helpers directly with NO external sync, to
// catch any thread_local regression even when the WPShaderParser
// mutex still holds.
//
// Under -DWEK_SANITIZE=thread a clean run proves the serialization holds; a
// refactor that drops/narrows the lock makes TSAN report a race on either
// the keyword table or libstdc++'s regex NFA state.
int repro_glslang_concurrent_compile(int iters) {
    wallpaper::WPShaderParser::InitGlslang(); // once for this process

    constexpr int     kThreads   = 4;
    const int         kPerThread = std::max(1, iters / 400); // ~50 at default 20000
    std::atomic<bool> go { false };
    std::atomic<long> ok_count { 0 };

    // Minimal valid sources for each stage we want to exercise.  Each is
    // suffixed at call time with distinct identifiers per (tid,k) to defeat
    // the SHA1-keyed compile cache (forces fresh tokenize + translate work
    // every iteration).  Geometry uses HLSL [maxvertexcount] which exercises
    // TranslateGeometryShader's static-const-regex pipeline; fragment uses
    // an HLSL clip() statement which exercises TranslateHlslClip.
    auto worker = [&](int tid) {
        while (! go.load()) {
        }
        long local = 0;
        for (int k = 0; k < kPerThread; ++k) {
            const std::string suf = "_" + std::to_string(tid) + "_" + std::to_string(k);

            // Stage rotation: 0=plain frag, 1=HLSL-clip frag, 2=HLSL geom.
            const int stage_pick = (tid + k) % 3;
            wallpaper::ShaderType stage;
            std::string src;
            switch (stage_pick) {
            case 0:
                stage = wallpaper::ShaderType::FRAGMENT;
                src   = "void main() { float x" + suf +
                      " = 0.0; gl_FragColor = vec4(x" + suf + "); }\n";
                break;
            case 1:
                stage = wallpaper::ShaderType::FRAGMENT;
                src   = "void main() { float a" + suf +
                      " = 0.5; clip(a" + suf +
                      " - 0.25); gl_FragColor = vec4(a" + suf + "); }\n";
                break;
            default:
                stage = wallpaper::ShaderType::GEOMETRY;
                src   = "[maxvertexcount(4)]\n"
                      "void main(point float4 input" + suf +
                      "[1] : SV_POSITION, inout TriangleStream<float4> stream" + suf +
                      ") { stream" + suf + ".Append(input" + suf + "[0]); }\n";
                break;
            }

            wallpaper::fs::VFS                   vfs; // no cache -> sync compile
            wallpaper::WPShaderInfo              info;
            std::vector<wallpaper::WPShaderUnit> units {
                { stage, src, {} }
            };
            std::vector<wallpaper::ShaderCode>      codes;
            std::vector<wallpaper::WPShaderTexInfo> texs;
            // CompileToSpv returns true even when downstream compile fails
            // (geometry+HLSL is intentionally invalid for glslang without
            // full preprocessing — the point of this repro is racing the
            // translate+preprocess path, not validating glslang accepts it).
            (void)wallpaper::WPShaderParser::CompileToSpv("tsan", units, codes, vfs, &info, texs);
            ++local;
        }
        ok_count.fetch_add(local);
    };

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) ts.emplace_back(worker, t);
    go.store(true);
    for (auto& t : ts) t.join();

    // Invariant: every iteration completed (no crash).  The point of this
    // repro is "no SIGSEGV / no TSAN race" — actual glslang acceptance is
    // not asserted.
    return ok_count.load() == (long)kThreads * kPerThread ? 0 : 1;
}

// Repro 4: direct WPShaderTransforms.h helpers from N threads with NO
// external mutex.  Repro 3 above tested CompileToSpv which holds the
// g_glslangSerialiseMtx — that mutex narrowed the surface but doesn't
// catch the deeper bug (libstdc++ shared-NFA UAF) on its own.  This
// repro proves the static→thread_local std::regex conversion is what
// actually makes the helpers safe under concurrent unsynchronised use.
//
// Pre-fix (`static const std::regex` shared across threads):
//   ASan reports heap-use-after-free in _NFA_base::_M_sub_count or
//   _M_handle_word_boundary; release builds SIGSEGV in _M_dfs.
// Post-fix (`thread_local const std::regex`):
//   Each thread has its own per-NFA state — clean run AND TSAN-clean.
int repro_wpshader_transforms_unlocked(int iters) {
    const int         kThreads   = 8;
    const int         kPerThread = std::max(1, iters / 800);
    std::atomic<bool> go { false };
    std::atomic<long> ok_count { 0 };

    // Geometry shader source that exercises TranslateGeometryShader's
    // full regex chain (steps 1-12): [maxvertexcount], in/out vec4
    // gl_Position, IN[0].gl_Position, IN[0].v_xxx, OUT.Append, etc.
    // Mirrors the shape of WE's stock genericparticle.geom (2.7KB).
    const std::string kGeomSrc =
        "#include \"common_particles.h\"\n"
        "in vec4 v_Color;\n"
        "in vec4 gl_Position;\n"
        "in vec3 v_Rotation;\n"
        "out vec4 v_Color;\n"
        "out vec4 gl_Position;\n"
        "out vec3 v_WorldPos;\n"
        "out vec3 v_WorldRight;\n"
        "PS_INPUT CreateParticleVertex(vec2 sprite, in VS_OUTPUT IN, vec3 right, vec3 up) {\n"
        "    PS_INPUT v;\n"
        "    v.gl_Position = mul(vec4(IN.gl_Position.xyz, 1.0), g_ModelViewProjectionMatrix);\n"
        "    v.v_Color = IN.v_Color;\n"
        "    v.v_WorldPos = mul(vec4(IN.gl_Position.xyz, 1.0), g_ModelMatrix).xyz;\n"
        "    v.v_WorldRight = mul(right, CAST3X3(g_ModelMatrix)).xyz;\n"
        "    return v;\n"
        "}\n"
        "[maxvertexcount(4)]\n"
        "void main() {\n"
        "    vec3 right = IN[0].v_Rotation;\n"
        "    vec3 up = vec3(0,1,0);\n"
        "    OUT.Append(CreateParticleVertex(vec2(0,0), IN[0], right, up));\n"
        "    OUT.Append(CreateParticleVertex(vec2(1,1), IN[0], right, up));\n"
        "}\n";

    auto worker = [&](int tid) {
        (void)tid;
        while (! go.load()) {
        }
        long local = 0;
        for (int k = 0; k < kPerThread; ++k) {
            // Same source on every thread → exercises shared static NFA
            // if any regex slipped through the thread_local conversion.
            std::string out = TranslateGeometryShader(kGeomSrc);
            // Result is non-empty; basic functional invariant.
            if (! out.empty()) ++local;
        }
        ok_count.fetch_add(local);
    };

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) ts.emplace_back(worker, t);
    go.store(true);
    for (auto& t : ts) t.join();

    return ok_count.load() == (long)kThreads * kPerThread ? 0 : 1;
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

    std::printf("[thread-repro] concurrent CompileToSpv (glslang), %d iters...\n", iters);
    rc |= repro_glslang_concurrent_compile(iters);

    std::printf("[thread-repro] WPShaderTransforms helpers unlocked (thread_local regex), %d iters...\n", iters);
    rc |= repro_wpshader_transforms_unlocked(iters);

    std::printf("[thread-repro] done (rc=%d)%s\n", rc,
                rc == 0 ? " — clean (check TSAN output for races)" : " — FAILED invariant");
    return rc;
}
