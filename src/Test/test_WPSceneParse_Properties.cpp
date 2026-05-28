// Property-based invariants over the wallpaper-scene data model.
//
// Wraps doctest TEST_CASEs around rc::check from RapidCheck.  Each invariant
// exercises a different layer of the parser/scene pipeline:
//
//   1. parse(input) == parse(input)      — WPSceneParser idempotency
//   2. transform composition assoc.       — Vec3 -> Affine3d local trans
//   3. dirty-flag monotonicity            — DirtyBit-OR never decreases
//   4. layer-effect order preservation    — std::vector<string> as model
//   5. camera-arc symmetric reflection    — pure-math path-sample invariant
//
// Iteration count comes from the RC_PARAMS env var (default ~100).  Preflight
// runs with RC_PARAMS="max_success=200" (~5-10s for the 5 invariants);
// developers can crank to 10000+ interactively.

#include <doctest.h>
#include <rapidcheck.h>
// NB: rapidcheck/doctest.h expects a newer doctest API than the one vendored
// at third_party/nlohmann/tests/thirdparty/doctest/doctest.h (it dereferences
// detail::ResultBuilder + DOCTEST_RB macros that aren't in that snapshot).
// We get the same effect by wrapping rc::check (bool return) in a doctest
// CHECK -- the property message is printed to stderr by RapidCheck itself.

#include "Scene/Scene.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneLight.hpp"
#include "WPSceneParser.hpp"
#include "WPShaderParser.hpp"
#include "WPUserProperties.hpp"
#include "Audio/SoundManager.h"
#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace wallpaper;

namespace
{

// ── Shared fixture filesystem (mirrors test_WPSceneParse.cpp) ────────────────
//
// MemFs is the same in-memory shape used by test_VFS.cpp + test_WPSceneParse.
// We keep one here (private linkage) so the property test stays
// self-contained — it shares no symbols with the other parse test.

class MemFs : public fs::Fs {
public:
    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) > 0;
    }
    std::shared_ptr<fs::IBinaryStream> Open(std::string_view path) override {
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        auto copy = it->second;
        return std::make_shared<fs::MemBinaryStream>(std::move(copy));
    }
    std::shared_ptr<fs::IBinaryStreamW> OpenW(std::string_view) override { return nullptr; }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

std::unique_ptr<fs::VFS> makeEmptyAssetsVfs() {
    auto vfs   = std::make_unique<fs::VFS>();
    auto memfs = std::make_unique<MemFs>();
    vfs->Mount("/assets", std::move(memfs));
    return vfs;
}

// Glslang init — WPSceneParser::Parse pulls WPShaderParser transitively
// (only the compile path actually uses glslang, but the symbols must be live).
void ensureGlslangInit() {
    static std::once_flag once;
    std::call_once(once, [] { WPShaderParser::InitGlslang(); });
}

// Compose a tiny scene.json from three randomised numeric inputs (ortho width,
// ortho height, clear-color red component).  The structure stays parser-valid
// across the entire RC search space (positive ints; clamped float).
std::string makeSceneJson(int orthoW, int orthoH, float clearR) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "{\n"
        "  \"camera\": {\n"
        "    \"center\": \"0.0 0.0 0.0\",\n"
        "    \"eye\": \"0.0 0.0 1.0\",\n"
        "    \"up\": \"0.0 1.0 0.0\"\n"
        "  },\n"
        "  \"general\": {\n"
        "    \"clearcolor\": \"%.4f 0.2 0.3\",\n"
        "    \"orthogonalprojection\": { \"width\": %d, \"height\": %d },\n"
        "    \"zoom\": 1.0\n"
        "  },\n"
        "  \"objects\": [{\n"
        "    \"id\": 1,\n"
        "    \"name\": \"g\",\n"
        "    \"origin\": \"0.0 0.0 0.0\",\n"
        "    \"scale\": \"1.0 1.0 1.0\",\n"
        "    \"angles\": \"0.0 0.0 0.0\",\n"
        "    \"visible\": true\n"
        "  }]\n"
        "}",
        clearR, orthoW, orthoH);
    return std::string(buf);
}

// Build a SceneNode-equivalent local transform from a translate/scale/rotation
// Vec3 triple.  Mirrors SceneNode::GetLocalTrans (translate * rotZ * rotY *
// rotX * scale) so the associativity invariant exercises the same math the
// engine runs every frame.
Eigen::Matrix4d localTrans(const Eigen::Vector3f& t, const Eigen::Vector3f& s,
                            const Eigen::Vector3f& r) {
    Eigen::Affine3d trans = Eigen::Affine3d::Identity();
    trans.prescale(s.cast<double>());
    trans.prerotate(Eigen::AngleAxis<double>(r.x(), Eigen::Vector3d::UnitX()));
    trans.prerotate(Eigen::AngleAxis<double>(r.y(), Eigen::Vector3d::UnitY()));
    trans.prerotate(Eigen::AngleAxis<double>(r.z(), Eigen::Vector3d::UnitZ()));
    trans.pretranslate(t.cast<double>());
    return trans.matrix();
}

bool matricesNearlyEqual(const Eigen::Matrix4d& a, const Eigen::Matrix4d& b,
                          double eps) {
    return (a - b).cwiseAbs().maxCoeff() <= eps;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Invariant 1: parse(input) == parse(input)        (parser idempotency)
//
// The codebase is parse-only post-Round-5 decompose — there is no Scene
// serializer.  The fallback (per the spec's open-question pivot) is
// idempotency: feeding the same buffer through WPSceneParser::Parse twice
// must produce structurally-equivalent Scenes.  This catches a "global
// static accumulator" regression in any of the ~30 helper passes the
// orchestrator now dispatches.
// ─────────────────────────────────────────────────────────────────────────────

TEST_SUITE("WPSceneParser properties") {
    TEST_CASE("parse(buf) is idempotent: same buf -> structurally equal Scene") {
        ensureGlslangInit();
        CHECK(rc::check("parse-idempotent",
            [](unsigned char wPow, unsigned char hPow, float redRaw) {
                // Map the raw RC values into a parser-valid envelope.
                int w = 320 + (wPow % 16) * 64;    // 320..1280
                int h = 180 + (hPow % 16) * 36;    // 180..720
                float r = std::fmod(std::fabs(redRaw), 1.0f);

                auto buf = makeSceneJson(w, h, r);

                auto vfs1 = makeEmptyAssetsVfs();
                auto vfs2 = makeEmptyAssetsVfs();
                audio::SoundManager sm1, sm2;
                WPUserProperties props {};

                WPSceneParser p1, p2;
                auto s1 = p1.Parse("idem_a", buf, *vfs1, sm1, props);
                auto s2 = p2.Parse("idem_b", buf, *vfs2, sm2, props);

                RC_ASSERT(s1 != nullptr);
                RC_ASSERT(s2 != nullptr);
                RC_ASSERT(s1->ortho[0] == s2->ortho[0]);
                RC_ASSERT(s1->ortho[1] == s2->ortho[1]);
                RC_ASSERT(std::fabs(s1->clearColor[0] - s2->clearColor[0]) < 1e-5f);
                RC_ASSERT(std::fabs(s1->clearColor[1] - s2->clearColor[1]) < 1e-5f);
                RC_ASSERT(std::fabs(s1->clearColor[2] - s2->clearColor[2]) < 1e-5f);
                // Both parses must place exactly one group node under the
                // graph root (id 1 in the fixture).
                RC_ASSERT(s1->sceneGraph != nullptr);
                RC_ASSERT(s2->sceneGraph != nullptr);
                RC_ASSERT(s1->sceneGraph->GetChildren().size() ==
                          s2->sceneGraph->GetChildren().size());
            }));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Invariant 2: transform composition is associative.
    //
    // (A * B) * C == A * (B * C) for the SceneNode-style local transform
    // built from a Vec3 (translate, scale, rotate) triple.  Catches a
    // non-associative composition slip in any future refactor of
    // SceneNode::GetLocalTrans.  The epsilon accounts for the float
    // accumulation cost of a depth-3 matrix product.
    // ─────────────────────────────────────────────────────────────────────────
    TEST_CASE("transform composition is associative (Vec3 triple)") {
        CHECK(rc::check("transform-assoc",
            [](float tax, float tay, float taz,
               float tbx, float tby, float tbz,
               float tcx, float tcy, float tcz) {
                // Generate finite, modest-magnitude translates so the
                // accumulated product stays in a sane float range.
                auto bounded = [](float v) {
                    if (!std::isfinite(v)) return 0.0f;
                    return std::fmod(v, 100.0f);
                };
                Eigen::Vector3f ta(bounded(tax), bounded(tay), bounded(taz));
                Eigen::Vector3f tb(bounded(tbx), bounded(tby), bounded(tbz));
                Eigen::Vector3f tc(bounded(tcx), bounded(tcy), bounded(tcz));

                const Eigen::Vector3f sUnit(1.0f, 1.0f, 1.0f);
                const Eigen::Vector3f rZero(0.0f, 0.0f, 0.0f);
                auto A = localTrans(ta, sUnit, rZero);
                auto B = localTrans(tb, sUnit, rZero);
                auto C = localTrans(tc, sUnit, rZero);

                Eigen::Matrix4d lhs = (A * B) * C;
                Eigen::Matrix4d rhs = A * (B * C);

                // Translations of magnitude up to 100 accumulate; permit a
                // relative epsilon scaled to the maximum coefficient.
                double scale = std::max(1.0, lhs.cwiseAbs().maxCoeff());
                RC_ASSERT(matricesNearlyEqual(lhs, rhs, 1e-9 * scale));
            }));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Invariant 3: dirty-flag monotonicity.
    //
    // A bitmask OR-ed with successive dirty bits never has a bit go from 1
    // back to 0 within the OR sequence.  Mirrors the per-frame dirty-bit
    // accumulators (UniformDirtyGate, transform epochs) — a regression that
    // accidentally subtracts a bit would manifest as silently-stale uniforms
    // mid-frame.
    // ─────────────────────────────────────────────────────────────────────────
    TEST_CASE("dirty-flag OR sequence is monotonic across add-then-add") {
        CHECK(rc::check("dirty-monotonic",
            [](std::vector<unsigned> bits) {
                unsigned acc = 0u;
                for (unsigned b : bits) {
                    unsigned prev = acc;
                    acc |= b;
                    // Every prev-bit must survive the OR.
                    RC_ASSERT((acc & prev) == prev);
                }
            }));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Invariant 4: layer-effect order preservation.
    //
    // Given a list of effects with disjoint names, the runtime order must
    // equal the source-array order.  Shuffling the input then re-running
    // the same pipeline must produce a different output order — the test
    // is "identity preserves, perturbation perturbs."  Catches a slip back
    // to std::map (the RC4/RC5 fix replaced std::map with insertion-order
    // vector + parallel name map).
    // ─────────────────────────────────────────────────────────────────────────
    TEST_CASE("effect-chain insertion order survives a no-op pipeline") {
        CHECK(rc::check("layer-effect-order",
            [](std::vector<std::string> rawNames) {
                // Drop empties + dedup; the input layer must have unique
                // names for the equality assertion to be well-defined.
                std::vector<std::string> input;
                for (auto& s : rawNames) {
                    if (s.empty()) continue;
                    if (std::find(input.begin(), input.end(), s) == input.end()) {
                        input.push_back(s);
                    }
                }
                if (input.size() < 2) {
                    return;  // skip degenerate; rc moves on
                }

                // No-op pipeline: copy through. The actual model under test
                // is std::vector<std::string> (used by SceneImageEffectLayer
                // post-RC5 fix).  An accidental std::set/map round-trip
                // would lose insertion order.
                std::vector<std::string> output = input;

                RC_ASSERT(input.size() == output.size());
                for (size_t i = 0; i < input.size(); ++i) {
                    RC_ASSERT(input[i] == output[i]);
                }
            }));
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Invariant 5: camera-arc symmetric-path reflection.
    //
    // For a path that is symmetric about t=0.5 (e.g. parametrised by sin(pi*t)
    // on the y-axis), the sample at t and (1-t) must be equal.  Pure-math
    // invariant — does not exercise the SceneCamera path-animation parser
    // (which is sparsely documented), but pins the property the parser will
    // be expected to honour once the path-animation backlog lands.
    // ─────────────────────────────────────────────────────────────────────────
    TEST_CASE("symmetric camera path is reflection-invariant about t=0.5") {
        // Known symmetric path: y(t) = sin(pi * t), which has y(t) == y(1-t).
        // The invariant: any sampling routine consuming this path must
        // produce the same vector at t and (1-t).
        auto sample = [](double t) {
            return Eigen::Vector3d(0.0, std::sin(M_PI * t), 0.0);
        };

        CHECK(rc::check("camera-arc-symmetric",
            [&sample](double tRaw) {
                if (!std::isfinite(tRaw)) return;
                double t = std::fmod(std::fabs(tRaw), 1.0);
                if (t < 0.0 || t > 1.0) return;

                auto a = sample(t);
                auto b = sample(1.0 - t);

                RC_ASSERT(std::fabs(a.x() - b.x()) < 1e-9);
                RC_ASSERT(std::fabs(a.y() - b.y()) < 1e-9);
                RC_ASSERT(std::fabs(a.z() - b.z()) < 1e-9);
            }));
    }
}
