#include <doctest.h>

// End-to-end exercise of the top-level scene-parser entry point
// `WPSceneParser::Parse(scene_id, buf, vfs, sm, userProps)`.
//
// Every other parser test drives a *helper* in isolation
// (test_WPSceneInitJsonHierarchy, test_WPSceneGroupParse, test_WPScene, the
// WPPropertyScriptExtract suite, …) and the fuzzer reaches only
// `wpscene::WPScene::FromJson` — the JSON root.  The ~1450-line assembly that
// turns a scene.json + assets into a `Scene` graph (two-pass group/image
// construction, camera/light/script wiring, Z-order restoration) is never run
// as a unit.  That assembly is the actual product spine and is where the
// long audit-chain regressions (group-parent, compose-dependency, Z-order)
// manifest.
//
// Asset seam: `Parse` reads *every* asset through `fs::VFS`
// (`fs::GetFileContent(vfs, "/assets/...")` / `vfs.Contains(...)`), so we feed
// it a fixture filesystem with zero real disk access.  The in-memory `Fs`
// pattern is the one already used by test_VFS.cpp — a `MemFs` mounted at
// `/assets`.
//
// The fixture deliberately uses only transform-only group nodes + a point
// light + a scripted `visible` property.  None of those touch
// `LoadMaterial` / shader translation, so the case stays CPU-only: it builds
// a `Scene` without compiling any SPIR-V and without a Vulkan device.

#include "WPSceneParser.hpp"
#include "test_scratch.hpp"
#include "WPShaderParser.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneImageEffectLayer.h"
#include "Scene/SceneLight.hpp"
#include "Scene/SceneMesh.h"
#include "Scene/SceneNode.h"
#include "SpecTexs.hpp"
#include "SystemFontFallback.hpp"
#include "WPUserProperties.hpp"

#include "Audio/SoundManager.h"

#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"
#include "Fs/PhysicalFs.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h> // getpid

using namespace wallpaper;

namespace
{

// Minimal in-memory Fs — same shape as test_VFS.cpp's MemFs.  Mount it at
// "/assets" so `fs::GetFileContent(vfs, "/assets/<x>")` resolves against the
// in-memory map and never hits disk.
class MemFs : public fs::Fs {
public:
    void add(std::string path, std::string data) {
        m_files[std::move(path)] = std::vector<uint8_t>(data.begin(), data.end());
    }
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

// Build a VFS with an empty /assets mount.  The fixture scene below references
// no shader/texture/material assets, so an empty mount is sufficient; callers
// that need util materials would `memfs->add(...)` before mounting.
std::unique_ptr<fs::VFS> makeEmptyAssetsVfs() {
    auto vfs     = std::make_unique<fs::VFS>();
    auto memfs   = std::make_unique<MemFs>();
    bool mounted = vfs->Mount("/assets", std::move(memfs));
    REQUIRE(mounted);
    return vfs;
}

// Process-wide glslang init.  CompileToSpv -> CompileShaderUnits needs the
// glslang globals.  Mirrors the std::call_once pattern in
// test_WarmCacheTexSlots.cpp + test_thread_repro.cpp; never paired with a
// FinalGlslang() — process exit handles teardown.
void ensureGlslangInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        WPShaderParser::InitGlslang();
    });
}

// Trivial GLSL pair shared by every E2E image/effect fixture below.  The WE
// preamble (WPShaderPreamble.hpp) injects `#define attribute in` for vertex
// and remaps `gl_FragColor` to an `out vec4` for fragment, so a "classic"
// `attribute vec3 a_Position` declaration + `gl_FragColor = vec4(1)` write
// compiles cleanly through PreShaderSrc + PreShaderHeader -> glslang.  We
// don't reference g_ModelViewProjectionMatrix here because the fixture's
// material has no textures and the renderer only needs valid SPV for
// LoadMaterial to succeed; the test never runs a Vulkan draw.  The vert
// must still write gl_Position so glslang doesn't reject it; we use a
// pass-through.
constexpr const char* kTrivialVert = R"GLSL(
attribute vec3 a_Position;
void main() {
    gl_Position = vec4(a_Position, 1.0);
}
)GLSL";

constexpr const char* kTrivialFrag = R"GLSL(
void main() {
    gl_FragColor = vec4(1.0);
}
)GLSL";

// Minimal `passes`-wrapped material JSON pointing at the trivial shader.
// WPMaterial::FromJson requires the outer `{ "passes": [ { "shader": "..."
// } ] }` shape (see wpscene/WPMaterial.cpp:84-93).
constexpr const char* kPlainMaterialJson = R"({
    "passes": [{
        "shader": "_t",
        "blending": "translucent",
        "textures": []
    }]
})";

// Plain image descriptor: 256x256 quad, references _plain material above.
// WPImageObject::FromJson reads `material` (mandatory), `width`/`height`
// (optional but used here to skip the autosize path) — see
// wpscene/WPImageObject.cpp:513-525.
constexpr const char* kPlainImageJson = R"({
    "material": "materials/_plain.json",
    "width": 256,
    "height": 256
})";

// Effect file referenced by the headline (a-2) case.  WPImageEffect::
// FromFileJson requires `name` + `passes` with each pass containing a
// `material` path (wpscene/WPImageObject.cpp:267-323).  The lone pass
// points at the same trivial-shader material so the effect's glslang
// compile succeeds.
constexpr const char* kEffectFileJson = R"({
    "name": "tint",
    "passes": [{
        "material": "materials/_plain.json"
    }]
})";

// Effect file whose first pass is a bare `command` rather than a material —
// the shape stock `motionblur` uses.  WPImageEffect::FromFileJson turns it
// into a WPEffectCommand, and the chain assembly must translate it into a
// SceneImageEffect::Command; without the copy, the accumulation pass samples
// an FBO nothing ever writes.
constexpr const char* kEffectWithCopyCommandJson = R"({
    "name": "trail",
    "fbos": [{ "name": "FullCompoBuffer1", "scale": 1 }],
    "passes": [
        { "command": "copy", "target": "FullCompoBuffer1", "source": "previous" },
        { "material": "materials/_plain.json" }
    ]
})";

// Build a /assets-mounted MemFs preloaded with the trivial shader pair, the
// shared plain image/material JSONs, and any extra (path, content) pairs
// each test wants to layer in.  VFS::GetPathInMount strips the "/assets"
// prefix at lookup time (see Fs/VFS.h:33-36), so MemFs keys are POST-
// strip — "/shaders/_t.vert" rather than "/assets/shaders/_t.vert".
//
// Confirmed by test_VFS.cpp ("Contains matches only paths under a mount
// point"): a MemFs that adds "/hello.txt" answers a vfs.Open("/assets/
// hello.txt").  Same key convention applies here.
std::unique_ptr<fs::VFS>
makeAssetsVfsWith(std::initializer_list<std::pair<std::string, std::string>> extras) {
    auto vfs   = std::make_unique<fs::VFS>();
    auto memfs = std::make_unique<MemFs>();

    // Trivial shader pair — every fixture below uses shader "_t".
    memfs->add("/shaders/_t.vert", kTrivialVert);
    memfs->add("/shaders/_t.frag", kTrivialFrag);

    // Text layers hard-code the "genericimage2" shader (buildTextWpMaterial),
    // so text fixtures need it under that name too.
    memfs->add("/shaders/genericimage2.vert", kTrivialVert);
    memfs->add("/shaders/genericimage2.frag", kTrivialFrag);

    // Shared plain image descriptor + material.
    memfs->add("/models/_plain.json", kPlainImageJson);
    memfs->add("/materials/_plain.json", kPlainMaterialJson);

    for (const auto& [path, content] : extras) {
        memfs->add(path, content);
    }

    REQUIRE(vfs->Mount("/assets", std::move(memfs)));
    return vfs;
}

// A tiny but *real* 2D scene.json:
//   - general: a non-default clearcolor + explicit ortho size (1280x720).
//   - objects:
//       id 100 "root_group"  — top-level transform group, scripted `visible`.
//       id 101 "child_group" — parented to 100 (exercises the group two-pass
//                              parent-link), origin offset.
//       id 200 "sun"         — point light (color/radius/intensity).
//
// No image/text/particle/sound/model objects, so the parse never reaches
// material/shader loading (stays Vulkan-free) but still drives the full
// camera/light/script/Z-order assembly.
const char* kFixtureSceneJson = R"JSON(
{
  "camera": {
    "center": "0.00000 0.00000 0.00000",
    "eye": "0.00000 0.00000 1.00000",
    "up": "0.00000 1.00000 0.00000"
  },
  "general": {
    "clearcolor": "0.10000 0.20000 0.30000",
    "orthogonalprojection": { "width": 1280, "height": 720 },
    "zoom": 1.0
  },
  "objects": [
    {
      "id": 100,
      "name": "root_group",
      "origin": "0.00000 0.00000 0.00000",
      "scale": "1.00000 1.00000 1.00000",
      "angles": "0.00000 0.00000 0.00000",
      "visible": {
        "script": "function update(v){ return v; }",
        "scriptproperties": {},
        "value": true
      }
    },
    {
      "id": 101,
      "name": "child_group",
      "parent": 100,
      "origin": "64.00000 32.00000 0.00000",
      "scale": "2.00000 2.00000 1.00000",
      "angles": "0.00000 0.00000 0.00000",
      "visible": true
    },
    {
      "id": 200,
      "name": "sun",
      "light": "point",
      "color": "1.00000 0.50000 0.25000",
      "origin": "640.00000 360.00000 200.00000",
      "scale": "1.00000 1.00000 1.00000",
      "angles": "0.00000 0.00000 0.00000",
      "radius": 1500.0,
      "intensity": 0.75,
      "visible": true
    }
  ]
}
)JSON";

} // namespace

TEST_SUITE("WPSceneParser::Parse (end-to-end)") {
    TEST_CASE("assembles a Scene from a minimal group+light scene.json") {
        auto                vfs = makeEmptyAssetsVfs();
        audio::SoundManager sm; // default-constructed: opens no device
        WPUserProperties    props {};

        WPSceneParser parser;
        auto          scene = parser.Parse("test_scene_42", kFixtureSceneJson, *vfs, sm, props);

        // ── Parse succeeded and produced a Scene ─────────────────────────────
        REQUIRE(scene != nullptr);
        CHECK(scene->scene_id == "test_scene_42");

        // ── General fields flowed through (catches a clearcolor/ortho
        //    assembly-order regression) ───────────────────────────────────────
        CHECK(scene->ortho[0] == 1280);
        CHECK(scene->ortho[1] == 720);
        CHECK(scene->clearColor[0] == doctest::Approx(0.10f));
        CHECK(scene->clearColor[1] == doctest::Approx(0.20f));
        CHECK(scene->clearColor[2] == doctest::Approx(0.30f));

        // ── A camera is present and active ──────────────────────────────────
        // 2D ortho scenes get a "global" + "effect" camera; activeCamera points
        // at "global".  (3D scenes add "global_ortho"/perspective variants.)
        CHECK(scene->cameras.count("global") == 1);
        CHECK(scene->cameras.count("effect") == 1);
        REQUIRE(scene->activeCamera != nullptr);
        CHECK(scene->activeCamera == scene->cameras.at("global").get());

        // ── The light was assembled with its authored params ────────────────
        REQUIRE(scene->lights.size() == 1);
        const auto& light = *scene->lights.front();
        CHECK(light.radius() == doctest::Approx(1500.0f));
        CHECK(light.intensity() == doctest::Approx(0.75f));
        CHECK(light.color().x() == doctest::Approx(1.00f));
        CHECK(light.color().y() == doctest::Approx(0.50f));
        CHECK(light.color().z() == doctest::Approx(0.25f));
        // The light owns a scene node attached under the graph root.
        CHECK(light.node() != nullptr);

        // ── Node graph: scene root exists and the group hierarchy is wired ───
        REQUIRE(scene->sceneGraph != nullptr);
        // The light's node was attached under the scene graph (ParseLightObj
        // AppendChild's it to the root).  (Lights are not name-addressable via
        // nodeNameToId — that map is for script-addressable image/text layers.)
        REQUIRE(light.node() != nullptr);
        bool light_node_in_graph = false;
        for (auto& c : scene->sceneGraph->GetChildren()) {
            if (c.get() == light.node()) light_node_in_graph = true;
        }
        CHECK(light_node_in_graph);

        // child_group (101) must be parented under root_group (100), not the
        // scene root — this is the group two-pass link that has regressed
        // before (solar-system info-panel collapse).
        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren()) {
                if (auto* hit = findById(c.get(), id)) return hit;
            }
            return nullptr;
        };
        SceneNode* root_group = findById(scene->sceneGraph.get(), 100);
        REQUIRE(root_group != nullptr);
        bool found_child_under_root = false;
        for (auto& c : root_group->GetChildren()) {
            if (c->ID() == 101) found_child_under_root = true;
        }
        CHECK(found_child_under_root);

        // ── A SceneScript was pre-scanned/extracted and wired ───────────────
        // The scripted `visible` on root_group (id 100) becomes a
        // ScenePropertyScript with property=="visible".
        REQUIRE(scene->propertyScripts.size() >= 1);
        bool has_visible_script = false;
        for (const auto& ps : scene->propertyScripts) {
            if (ps.id == 100 && ps.property == "visible") {
                has_visible_script = true;
                CHECK(ps.layerName == "root_group");
                CHECK(ps.script.find("update") != std::string::npos);
            }
        }
        CHECK(has_visible_script);
    }

    TEST_CASE("returns nullptr on malformed JSON") {
        auto                vfs = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};

        WPSceneParser parser;
        // PARSE_JSON fails → Parse bails at the very top with nullptr.  Cover
        // the three malformed shapes a hostile/truncated workshop scene.json
        // can take; each must yield nullptr (not a partial Scene) so the
        // MainHandler::loadScene call-site guard has a contract to lean on.
        //   - unterminated object (survives trailing-comma/leading-zero recovery)
        CHECK(parser.Parse("bad", "{ \"general\": ", *vfs, sm, props) == nullptr);
        //   - free-text, no JSON structure at all
        CHECK(parser.Parse("notjson", "{ this is not json", *vfs, sm, props) == nullptr);
        //   - empty buffer
        CHECK(parser.Parse("empty", "", *vfs, sm, props) == nullptr);
    }

    // Three transform-only groups declared in non-monotonic JSON order (30, 10,
    // 20).  The render order of the scene root's children must follow the
    // authored JSON "objects" order — the parser re-sorts root children to JSON
    // order at the end of Parse (WPSceneParser.cpp:4430) so the two-pass
    // group/image construction can't scramble Z-order (blue-archive sortLayer
    // regression).  Groups keep this case glslang-free.
    // Parented-light contract — Real-Time Earth (3557068717) drives this:
    // the wallpaper authors 2 point lights as children of the animated SUN m5
    // node (origin scripted from shared.sun_pos_*).  Before the parent+exponent
    // honoring, ParseLightObj appended every light directly to sceneGraph and
    // never read the JSON `parent` field — so parented lights collapsed at
    // world origin (Earth's center) yielding pure-black planet surfaces.
    //
    // This test exercises the full end-to-end pipeline: WPLightObject parsing →
    // ParseLightObj parent-chain lookup → SceneNode parent linkage → world
    // matrix walk.  The fixture mirrors the structural shape of the
    // Real-Time Earth scene (group + parented light + exponent) without any
    // material/shader assets so the test stays Vulkan-free.
    TEST_CASE("ParseLightObj honors JSON `parent` and exponent") {
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 99, "name": "GROUP",
      "origin": "10 20 30", "scale": "1 1 1", "angles": "0 0 0",
      "solid": true, "visible": true },
    { "id": 272, "name": "",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "parent": 99,
      "light": "lpoint",
      "color": "1 1 1",
      "radius": 3000.0, "intensity": 1.0, "exponent": 0.1,
      "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_parented_light", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 1);

        const auto& light = *scene->lights.front();
        // Exponent flowed through WPLightObject → SceneLight constructor.
        CHECK(light.exponent() == doctest::Approx(0.1f));

        // The light's node MUST be a child of the GROUP node (id 99), NOT a
        // direct child of sceneGraph.  Before the fix, every light went under
        // sceneGraph regardless of `parent:` and this assertion fails.
        auto* lnode = light.node();
        REQUIRE(lnode != nullptr);
        REQUIRE(lnode->Parent() != nullptr);

        // Walk down sceneGraph to find the GROUP and confirm the light node
        // sits beneath it.
        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren()) {
                if (auto* hit = findById(c.get(), id)) return hit;
            }
            return nullptr;
        };
        SceneNode* group = findById(scene->sceneGraph.get(), 99);
        REQUIRE(group != nullptr);
        bool light_under_group = false;
        for (auto& c : group->GetChildren()) {
            if (c.get() == lnode) light_under_group = true;
        }
        CHECK(light_under_group);

        // After UpdateTrans(), the world matrix's translation column equals
        // the parent's translation (since the light's local origin is 0,0,0).
        // This is what WPShaderValueUpdater must upload into g_LightsPosition
        // instead of the light's local Translate().
        lnode->UpdateTrans();
        const auto& m = lnode->ModelTrans();
        CHECK(m(0, 3) == doctest::Approx(10.0f));
        CHECK(m(1, 3) == doctest::Approx(20.0f));
        CHECK(m(2, 3) == doctest::Approx(30.0f));
    }

    TEST_CASE("multi-object scene: root child render order follows JSON order") {
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 30, "name": "c", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true },
    { "id": 10, "name": "a", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true },
    { "id": 20, "name": "b", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true }
  ]
}
)JSON";
        auto                vfs        = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_zorder", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->sceneGraph != nullptr);

        // The root also carries camera/wrapper nodes, so filter to our three
        // group ids and assert their RELATIVE order is the JSON declaration
        // order (30, 10, 20) — not ascending id, not some construction order.
        auto indexOf = [&](i32 id) -> long {
            long i = 0;
            for (auto& c : scene->sceneGraph->GetChildren()) {
                if (c->ID() == id) return i;
                ++i;
            }
            return -1;
        };
        const long i30 = indexOf(30), i10 = indexOf(10), i20 = indexOf(20);
        REQUIRE(i30 >= 0);
        REQUIRE(i10 >= 0);
        REQUIRE(i20 >= 0);
        CHECK(i30 < i10); // JSON order: 30 declared before 10
        CHECK(i10 < i20); // 10 declared before 20
    }

    TEST_CASE("ParseLightObj reads density and routes through SceneLight predicate") {
        // Mirrors a real preview scene: density=7.48, volumetricsexponent=4.0,
        // no explicit castvolumetrics (heuristic opts in via density>0).
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 50, "name": "preview_light",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "lpoint",
      "color": "1 1 1",
      "radius": 500.0, "intensity": 1.0,
      "density": 7.48,
      "volumetricsexponent": 4.0,
      "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_density", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 1);
        const auto& light = *scene->lights.front();
        CHECK(light.kind() == SceneLight::LightKind::LPoint);
        CHECK(light.volumetric().density == doctest::Approx(7.48f));
        CHECK(light.volumetric().exponent == doctest::Approx(4.0f));
        CHECK(light.volumetric().cast_volumetrics_explicit == false);
        CHECK(light.castsVolumetrics() == true); // density>0 heuristic
    }

    TEST_CASE("ParseLightObj honors explicit castvolumetrics:false despite density>0") {
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 50, "name": "off_light",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "lpoint",
      "color": "1 1 1",
      "radius": 500.0, "intensity": 1.0,
      "castvolumetrics": false,
      "density": 5.0,
      "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_off", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 1);
        const auto& light = *scene->lights.front();
        CHECK(light.volumetric().cast_volumetrics_explicit == true);
        CHECK(light.volumetric().cast_volumetrics_value == false);
        CHECK(light.castsVolumetrics() == false);
    }

    TEST_CASE("ParseLightObj parses ltube / ldirectional / lspot kinds") {
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 10, "name": "t", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "ltube",        "color": "1 1 1", "radius": 100.0, "intensity": 1.0, "visible": true },
    { "id": 11, "name": "d", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "ldirectional", "color": "1 1 1", "radius": 100.0, "intensity": 1.0, "visible": true },
    { "id": 12, "name": "s", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "lspot",        "color": "1 1 1", "radius": 100.0, "intensity": 1.0, "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_kinds", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 3);
        CHECK(scene->lights[0]->kind() == SceneLight::LightKind::LTube);
        CHECK(scene->lights[1]->kind() == SceneLight::LightKind::LDirectional);
        CHECK(scene->lights[2]->kind() == SceneLight::LightKind::LSpot);
    }

    TEST_CASE("ParseLightObj falls back to Point on unknown kind string") {
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 10, "name": "u", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "unobtainable",
      "color": "1 1 1", "radius": 100.0, "intensity": 1.0, "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_unknown_kind", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 1);
        CHECK(scene->lights[0]->kind() == SceneLight::LightKind::Point);
    }

    TEST_CASE("ParseLightObj reads castshadow and cascade distances") {
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 10, "name": "shadow_light",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "lpoint",
      "color": "1 1 1", "radius": 100.0, "intensity": 1.0,
      "castshadow": true,
      "cascadedistance0": 10.0,
      "cascadedistance1": 50.0,
      "cascadedistance2": 300.0,
      "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_shadow", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 1);
        const auto& light = *scene->lights.front();
        CHECK(light.castShadow() == true);
        CHECK(light.cascadeDistances()[0] == doctest::Approx(10.0f));
        CHECK(light.cascadeDistances()[1] == doctest::Approx(50.0f));
        CHECK(light.cascadeDistances()[2] == doctest::Approx(300.0f));
    }

    TEST_CASE("collisionmodel preview light parses with density=7.48, exp=4.0, exp(direct)=2.0") {
        // Canonical reference — built-in WE preview at
        // assets/scenes/particleelementpreviews/collisionmodel/scene.json.  The
        // light is an lpoint with all volumetric fields populated and the
        // cascade defaults (0/100/200).
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1920, "height": 1080 } },
  "objects": [
    { "id": 237, "name": "",
      "origin": "611.30676 302.13736 2000.0",
      "scale": "1 1 1",
      "angles": "0 0 0",
      "light": "lpoint",
      "color": "1.00000 0.95686 0.87451",
      "radius": 3000.0,
      "intensity": 0.5,
      "exponent": 2.0,
      "density": 7.48,
      "volumetricsexponent": 4.0,
      "castshadow": true,
      "cascadedistance0": 0.0,
      "cascadedistance1": 100.0,
      "cascadedistance2": 200.0,
      "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_collisionmodel", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 1);
        const auto& light = *scene->lights.front();
        CHECK(light.kind() == SceneLight::LightKind::LPoint);
        CHECK(light.exponent() == doctest::Approx(2.0f));
        CHECK(light.radius() == doctest::Approx(3000.0f));
        CHECK(light.intensity() == doctest::Approx(0.5f));
        CHECK(light.volumetric().density == doctest::Approx(7.48f));
        CHECK(light.volumetric().exponent == doctest::Approx(4.0f));
        CHECK(light.volumetric().cast_volumetrics_explicit == false);
        CHECK(light.castsVolumetrics() == true);
        CHECK(light.castShadow() == true);
        CHECK(light.cascadeDistances()[0] == doctest::Approx(0.0f));
        CHECK(light.cascadeDistances()[1] == doctest::Approx(100.0f));
        CHECK(light.cascadeDistances()[2] == doctest::Approx(200.0f));
    }

    TEST_CASE("workshop 3287715210 light parses with explicit castvolumetrics=true") {
        // Workshop 3287715210 (发光少女 4K) — the only scene in the inventoried
        // corpus carrying castvolumetrics: true explicitly.  Density + exp are
        // the author-tuned values.
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1920, "height": 1080 } },
  "objects": [
    { "id": 100, "name": "stage_light",
      "origin": "0 200 0",
      "scale": "1 1 1",
      "angles": "0 0 0",
      "light": "lpoint",
      "color": "1.0 0.8 0.6",
      "radius": 1500.0,
      "intensity": 1.5,
      "castvolumetrics": true,
      "density": 0.65,
      "volumetricsexponent": 1.7,
      "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_ws_3287715210", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 1);
        const auto& light = *scene->lights.front();
        CHECK(light.kind() == SceneLight::LightKind::LPoint);
        CHECK(light.volumetric().cast_volumetrics_explicit == true);
        CHECK(light.volumetric().cast_volumetrics_value == true);
        CHECK(light.volumetric().density == doctest::Approx(0.65f));
        CHECK(light.volumetric().exponent == doctest::Approx(1.7f));
        CHECK(light.castsVolumetrics() == true);
    }

    TEST_CASE("real-time earth 3557068717 light parses with author-tuned density 0.31") {
        // Workshop 3557068717 (Real-Time Earth) — one of the scenes in the
        // inventoried corpus with volumetric fields.  Density 0.31 + exponent
        // ~1.54.  Light is one of the sun lights parented to the SUN node;
        // this test slices just the light entry (no parenting) since the
        // parent linkage is exercised by the pre-existing parented-light test
        // above.
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1920, "height": 1080 } },
  "objects": [
    { "id": 272, "name": "sun_light_a",
      "origin": "0 0 0",
      "scale": "1 1 1",
      "angles": "0 0 0",
      "light": "lpoint",
      "color": "1.0 0.97 0.85",
      "radius": 5000.0,
      "intensity": 3.0,
      "exponent": 0.1,
      "density": 0.31,
      "volumetricsexponent": 1.54,
      "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_ws_3557068717", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 1);
        const auto& light = *scene->lights.front();
        CHECK(light.exponent() == doctest::Approx(0.1f));
        CHECK(light.volumetric().density == doctest::Approx(0.31f));
        CHECK(light.volumetric().exponent == doctest::Approx(1.54f));
        CHECK(light.castsVolumetrics() == true);
    }

    TEST_CASE("Scene::volumetricsConfig flips chain off when shader assets are absent") {
        // With the post-parse material-attach step (LoadMaterial against the
        // VFS) failing for an empty assets mount, the chain gracefully
        // disables itself — the per-light enumeration ran and produced the
        // expected entry count, but the chain was wired down because
        // volumetricsback/volumetricsfront/blur_k3/passthrough shaders
        // weren't loadable.  Production wallpapers always carry the WE shader
        // assets so the chain stays enabled there; this test simply locks
        // the no-asset failure path so a malformed install can't crash.
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 10, "name": "off", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "lpoint", "color": "1 1 1", "radius": 100.0, "intensity": 1.0,
      "visible": true },
    { "id": 11, "name": "on", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "lpoint", "color": "1 1 1", "radius": 100.0, "intensity": 1.0,
      "density": 5.0,
      "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_enabled", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        // The per-light enumeration ran during parse — failOut in
        // attachVolumetricMaterials then cleared per_light + disabled the
        // chain because the empty VFS has no shader files.  Predicate-only
        // observable: enabled is now false, per_light is empty.
        CHECK(scene->volumetricsConfig.enabled == false);
        CHECK(scene->volumetricsConfig.per_light.empty());
    }

    TEST_CASE("volumetric materials get their SPV on the FIRST load") {
        // The volumetric chain is built and its materials compiled AFTER the
        // rest of the scene, so it is the one shader consumer that an
        // end-of-parse step running too early can skip entirely.  Drive a real
        // parse with a mounted SPV cache (the production configuration) and
        // require every volumetric node to come out with usable SPV.
        ensureGlslangInit();

        const std::string cache_dir = wallpaper::test::ScratchDir("volumetric_spv", ::getpid());
        std::filesystem::remove_all(cache_dir);
        std::filesystem::create_directories(cache_dir);

        const char* kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 10, "name": "fog", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "lpoint", "color": "1 1 1", "radius": 100.0, "intensity": 1.0,
      "castvolumetrics": true,
      "density": 5.0,
      "visible": true }
  ]
}
)JSON";

        auto vfs = makeAssetsVfsWith({
            { "/shaders/volumetricsback.vert", kTrivialVert },
            { "/shaders/volumetricsback.frag", kTrivialFrag },
            { "/shaders/volumetricsfront.vert", kTrivialVert },
            { "/shaders/volumetricsfront.frag", kTrivialFrag },
            { "/shaders/blur_k3.vert", kTrivialVert },
            { "/shaders/blur_k3.frag", kTrivialFrag },
            { "/shaders/passthrough.vert", kTrivialVert },
            { "/shaders/passthrough.frag", kTrivialFrag },
        });
        REQUIRE(vfs->Mount("/cache", fs::CreatePhysicalFs(cache_dir, true), "cache"));
        REQUIRE(vfs->IsMounted("cache"));

        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_volumetric_spv", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->volumetricsConfig.enabled);
        REQUIRE(scene->volumetricsConfig.per_light.size() == 1);

        auto spvOf = [](const std::shared_ptr<SceneNode>& n) -> size_t {
            if (! n || ! n->Mesh() || ! n->Mesh()->Material()) return 0;
            const auto& sh = n->Mesh()->Material()->customShader.shader;
            return sh ? sh->codes.size() : 0;
        };

        const auto& pl = scene->volumetricsConfig.per_light.front();
        CHECK(spvOf(pl.back_node) > 0);
        CHECK(spvOf(pl.front_node) > 0);
        CHECK(spvOf(pl.fullscreen_node) > 0);
        CHECK(spvOf(scene->volumetricsConfig.blur_h_node) > 0);
        CHECK(spvOf(scene->volumetricsConfig.blur_v_node) > 0);
        CHECK(spvOf(scene->volumetricsConfig.combine_node) > 0);

        std::filesystem::remove_all(cache_dir);
    }

    TEST_CASE("Scene::volumetricsConfig.enabled stays false when no light casts") {
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 10, "name": "off", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "lpoint", "color": "1 1 1", "radius": 100.0, "intensity": 1.0,
      "visible": true }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_disabled", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        CHECK(scene->volumetricsConfig.enabled == false);
    }

    // ────────────────────────────────────────────────────────────────────
    // Partials: image+effect-chain and compose-dependency end-to-end
    //
    // Every prior case in this suite stops at the group/light/script
    // boundary — none reaches ParseImageObj.  These four close the two
    // gaps the umbrella spec deferred:
    //
    //  - (a-1) image layer with NO effects — base ParseImageObj path,
    //          JSON transform propagates, no synthesised effect child.
    //  - (a-2) image layer with a real effect chain — image worldNode
    //          collapses to IDENTITY for the effect base-pass capture,
    //          the effect-layer FinalNode preserves the JSON transform
    //          (regression net: a script-driven `origin` must redirect
    //          to ResolvedLastOutput, not the worldNode).
    //  - (b-1) compose layer with `dependencies: [N]` — image N must be
    //          forced offscreen so the compose blend samples an isolated
    //          sprite RT (3498984739 gray-quad regression).
    //  - (b-2) non-compose image with `dependencies: [self, self]` —
    //          dropped by CollectComposeDependencyIds' two filters,
    //          dependent NOT offscreen (1210462523 Eclipse black-screen
    //          regression).
    //
    // The fixture VFS chain is exactly what runtime sees:
    //   scene.json -> image descriptor -> material -> shader pair.
    // All four cases share the makeAssetsVfsWith(...) scaffolding plus
    // the trivial GLSL pair (which glslang-compiles in <20ms after the
    // call_once init).
    // ────────────────────────────────────────────────────────────────────

    TEST_CASE("E2E: image layer with empty effects array (a-1)") {
        ensureGlslangInit();
        auto vfs = makeAssetsVfsWith({});

        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 300, "name": "the_image_name",
      "image": "models/_plain.json",
      "origin": "10 20 0",
      "scale":  "2.0 2.0 1.0",
      "angles": "0 0 0.7853981",
      "visible": true,
      "effects": [] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_image_no_effect", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        // Walk the graph to find id=300 — same recursive findById pattern
        // used by the earlier group-parent case.
        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren()) {
                if (auto* hit = findById(c.get(), id)) return hit;
            }
            return nullptr;
        };
        SceneNode* node = findById(scene->sceneGraph.get(), 300);
        REQUIRE(node != nullptr);
        CHECK(scene->nodeNameToId.count("the_image_name") == 1);

        // JSON transform propagated onto the worldNode (no effects = no
        // identity collapse).  Pin the three components separately so a
        // future regression names the broken axis.
        CHECK(node->Translate().x() == doctest::Approx(10.0f));
        CHECK(node->Translate().y() == doctest::Approx(20.0f));
        CHECK(node->Scale().x() == doctest::Approx(2.0f));
        CHECK(node->Scale().y() == doctest::Approx(2.0f));
        CHECK(node->Rotation().z() == doctest::Approx(0.7853981f).epsilon(1e-4));

        // No effects -> no nodeEffectLayerMap entry, no offscreen routing.
        CHECK(scene->nodeEffectLayerMap.count(300) == 0);
        CHECK(node->IsOffscreen() == false);
    }

    TEST_CASE("E2E: image layer with real effect chain — worldNode collapses to IDENTITY (a-2)") {
        ensureGlslangInit();
        auto vfs = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
        });

        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 301, "name": "img_with_effect",
      "image": "models/_plain.json",
      "origin": "10 20 0",
      "scale":  "2.0 2.0 1.0",
      "angles": "0 0 0.7853981",
      "visible": true,
      "effects": [
        { "id": 10, "name": "tint", "visible": true,
          "file": "effects/tint.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_image_effect_chain", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren()) {
                if (auto* hit = findById(c.get(), id)) return hit;
            }
            return nullptr;
        };
        SceneNode* node = findById(scene->sceneGraph.get(), 301);
        REQUIRE(node != nullptr);

        // The image is registered by name; the effect is addressable via the
        // layerEffectNames map instead, NOT as its own scene-graph node.
        CHECK(scene->nodeNameToId.count("img_with_effect") == 1);
        CHECK(scene->nodeNameToId.count("tint") == 0);

        // assembleEffectChain installs a SceneImageEffectLayer entry against
        // the image id (WPSceneParser.cpp:2327).  It owns the FinalNode that
        // carries the JSON transform; the worldNode itself is reset to
        // IDENTITY at WPSceneParser.cpp:2323 so the base capture pass draws
        // into the pingpong RT at origin.  This is the pin for the
        // property-anim-effect-layer-redirect regression: a vec3 origin
        // script on the image must update ResolvedLastOutput, NOT the
        // worldNode (which would put the geometry off-screen).
        REQUIRE(scene->nodeEffectLayerMap.count(301) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(301);
        REQUIRE(effLayer != nullptr);

        // worldNode collapsed to identity — confirms the CopyTrans(SceneNode())
        // reset fired on the non-compose, hasEffect path.
        CHECK(node->Translate().x() == doctest::Approx(0.0f));
        CHECK(node->Translate().y() == doctest::Approx(0.0f));
        CHECK(node->Scale().x() == doctest::Approx(1.0f));
        CHECK(node->Scale().y() == doctest::Approx(1.0f));
        CHECK(node->Rotation().z() == doctest::Approx(0.0f));

        // FinalNode carries the JSON transform (CopyTrans(*spImgNode) at
        // WPSceneParser.cpp:2290).  This is the corner the script redirect
        // must target.
        const SceneNode& finalNode = effLayer->FinalNode();
        CHECK(finalNode.Translate().x() == doctest::Approx(10.0f));
        CHECK(finalNode.Translate().y() == doctest::Approx(20.0f));
        CHECK(finalNode.Scale().x() == doctest::Approx(2.0f));
        CHECK(finalNode.Rotation().z() == doctest::Approx(0.7853981f).epsilon(1e-4));

        // Effect ordering / count: the visible effect produced one entry in
        // the layer's effect vector.  No effect was named "tint" in
        // nodeNameToId (confirmed above) — naming flows through
        // layerEffectNames instead.
        CHECK(effLayer->EffectCount() == 1);

        // The image is not offscreen (visible=true, not a compose dep).
        CHECK(node->IsOffscreen() == false);
        CHECK(effLayer->IsOffscreen() == false);
    }

    TEST_CASE("E2E: plain child of an effect parent inherits the parent world") {
        // A parent image with its OWN non-compose effect has its worldNode
        // reset to identity for the base capture pass (a-2 above).  A plain
        // (effect-less) child must still land at parentWorld * childLocal — it
        // gets a transform proxy carrying the parent's preserved world.  Before
        // the fix it chained through the identity-reset parent and sat at its
        // bare local offset (Hoshi-Tele 3042492564: the puppet's head + legs,
        // the only effect-less parts, piled in the lower-left corner).
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 401, "name": "parent_fx", "image": "models/_plain.json",
      "origin": "100 50 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [ { "id": 10, "name": "tint", "visible": true,
                     "file": "effects/tint.json" } ] },
    { "id": 402, "name": "plain_child", "parent": 401,
      "image": "models/_plain.json",
      "origin": "10 20 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_plain_child_fx_parent", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        // Parent has a non-compose effect → its worldNode collapsed to identity.
        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);

        auto cit = scene->nodeById.find(402);
        REQUIRE(cit != scene->nodeById.end());
        SceneNode* child = cit->second;
        REQUIRE(child != nullptr);
        child->UpdateTrans();
        const auto& m = child->ModelTrans();
        // world = parentWorld(100,50) * childLocal(10,20) = (110,70).
        // Stranded (pre-fix) it would read the bare local (10,20).
        CHECK(m(0, 3) == doctest::Approx(110.0));
        CHECK(m(1, 3) == doctest::Approx(70.0));
    }

    TEST_CASE("E2E: compose-dependency forces dependent image offscreen (b-1)") {
        ensureGlslangInit();
        // Build a compose-layer image descriptor in the VFS:
        // /assets/models/util/composelayer.json — the marker the parser
        // looks for in CollectComposeDependencyIds (WPImageObject.h:145).
        //
        // A no-effect compose layer also makes the parser synthesise a
        // passthrough effect from /assets/materials/util/effectpassthrough.json
        // (WPSceneParser.cpp:1567) — without it synthesizePassthroughForCompose
        // returns nullopt and the compose layer is dropped before reaching the
        // scene graph.  Use the same minimal trivial-shader material content
        // so the synthesised effect glslang-compiles cleanly.
        auto vfs = makeAssetsVfsWith({
            { "/models/util/composelayer.json", kPlainImageJson },
            { "/materials/util/effectpassthrough.json", kPlainMaterialJson },
        });

        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 401, "name": "dep_image",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true },
    { "id": 402, "name": "compose_layer",
      "image": "models/util/composelayer.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "dependencies": [401] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_compose_dep", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren()) {
                if (auto* hit = findById(c.get(), id)) return hit;
            }
            return nullptr;
        };
        SceneNode* dep     = findById(scene->sceneGraph.get(), 401);
        SceneNode* compose = findById(scene->sceneGraph.get(), 402);
        REQUIRE(dep != nullptr);
        REQUIRE(compose != nullptr);

        // The observable effect of CollectComposeDependencyIds picking up
        // 401: applyImagePreRoutingDefaults/computeOffscreenRouting at
        // WPSceneParser.cpp:1428 force the dependent image offscreen so the
        // compose blend samples an isolated sprite RT.  Without this, 401
        // renders to _rt_default and the compose layer paints solid quads
        // sampled from full-FB UVs (Clair Obscur Expedition 33 3498984739
        // gray-quads-over-characters regression).
        CHECK(dep->IsOffscreen() == true);

        // Sibling effect of the offscreen routing: ensureBareDependencyOffscreenRT
        // registers a /_rt_offscreen_<id>/ render target so the compose
        // blend's link-tex resolves to a real RT (WPSceneParser.cpp:2583).
        CHECK(scene->renderTargets.count(GenOffscreenRT(401)) == 1);
    }

    TEST_CASE("E2E: self-referential dependency dropped, dependent NOT offscreen (b-2)") {
        ensureGlslangInit();
        // No compose-layer marker in the VFS — the image at id 64 is a
        // plain (non-compose) image; CollectComposeDependencyIds' first
        // filter rejects it.  The fixture mirrors Eclipse 1210462523's
        // `dependencies:[64,64,64]` self-reference shape exactly.
        auto vfs = makeAssetsVfsWith({});

        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 64, "name": "self_ref",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "dependencies": [64, 64, 64] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_self_ref", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren()) {
                if (auto* hit = findById(c.get(), id)) return hit;
            }
            return nullptr;
        };
        SceneNode* node = findById(scene->sceneGraph.get(), 64);
        REQUIRE(node != nullptr);

        // The non-compose filter (WPImageObject.h:166-177) drops every
        // dependency from a non-compose image, and the self-reference
        // filter drops `dep_id == img->id` even if the image is a compose
        // layer.  Either way, 64 must NOT end up routed offscreen — the
        // Eclipse 1210462523 regression was exactly the inverse: 64 was
        // forced offscreen, nothing read the offscreen RT, and the screen
        // stayed at clearColor.
        CHECK(node->IsOffscreen() == false);
        // No offscreen RT registered for the self-ref id.
        CHECK(scene->renderTargets.count(GenOffscreenRT(64)) == 0);
    }

    // A text layer's effect chain has to come out of the parser with the same
    // shape an image layer's does: copy/swap commands translated, the effect
    // named, and the name published for SceneScript's getEffect().
    TEST_CASE("E2E: text layer effect keeps its copy command and registers its name") {
        ensureGlslangInit();
        // FreeType needs a real face; the box supplies one via the
        // systemfont_* fallback resolver.
        if (ResolveSystemFontFallback("systemfont_sans").empty()) return;

        auto vfs = makeAssetsVfsWith({
            { "/effects/trail.json", kEffectWithCopyCommandJson },
        });

        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 501, "name": "clock_text",
      "origin": "100 200 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "256 64",
      "font": "systemfont_sans", "pointsize": 24,
      "text": { "value": "12:00" },
      "visible": true,
      "effects": [
        { "id": 11, "name": "trail", "visible": true,
          "file": "effects/trail.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_text_effect_chain", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(501) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(501);
        REQUIRE(effLayer != nullptr);
        REQUIRE(effLayer->EffectCount() == 1);

        const auto& eff = effLayer->GetEffect(0);
        REQUIRE(eff != nullptr);

        // Named effects are what getEffect(name) / getEffectCount() resolve
        // against on the script side.
        CHECK(eff->name == "trail");
        CHECK(scene->layerEffectNames.count("clock_text") == 1);
        CHECK(scene->layerEffectNames["clock_text"] == std::vector<std::string> { "trail" });

        // The effect file's `command` pass must survive into the chain, or the
        // accumulation pass reads an FBO nothing wrote and the effect is dead.
        REQUIRE(eff->commands.size() == 1);
        CHECK(eff->commands.at(0).cmd == SceneImageEffect::CmdType::Copy);
        CHECK(eff->commands.at(0).src == effLayer->FirstTarget());
        CHECK(eff->commands.at(0).dst.find("FullCompoBuffer1") != std::string::npos);
        CHECK(scene->renderTargets.count(eff->commands.at(0).dst) == 1);
    }

} // TEST_SUITE

// ---------------------------------------------------------------------------
// Fuzz crash regression replay.
//
// Iterates tests/fixtures/fuzz_regressions/WPSceneParser/*.bin and feeds each
// file through the same entry point fuzz_WPSceneParser drives — JSON parse
// then wpscene::WPScene::FromJson. Production wraps FromJson in a no-throw
// envelope, so we mirror the fuzz harness's swallow.
// ---------------------------------------------------------------------------

#include "test_data_root.hpp"

#include "wpscene/WPScene.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

TEST_SUITE("WPSceneParser_Hotplug") {
    // Cooperative-cancellation of an in-flight scene parse.  The parser polls
    // an external std::atomic_bool* at hot-path checkpoints (Parse entry and
    // per-object dispatch).  When the flag is set BEFORE Parse runs, the
    // parser bails immediately with nullptr.  When set after a partial parse,
    // dispatch loop unwinds early.
    TEST_CASE("Parse with abort flag set at entry returns nullptr") {
        auto                vfs = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};

        std::atomic_bool abort_flag { true };
        WPSceneParser    parser;
        parser.SetAbortFlag(&abort_flag);

        auto scene = parser.Parse("aborted_at_entry", kFixtureSceneJson, *vfs, sm, props);
        CHECK(scene == nullptr);
        CHECK(parser.IsAborted() == true);
    }

    // A screen removal that lands *during* object dispatch, not before it.
    // dispatchObjects stops enumerating and hands back only the objects it
    // built, so the scene is missing every layer after the abort point --
    // nothing reloads it, so the user would stare at that truncated wallpaper
    // for the rest of the session and getLayer() would resolve to nothing for
    // the missing names.  Parse must throw the half-built scene away.
    TEST_CASE("Parse aborted mid-dispatch discards the half-built scene") {
        ensureGlslangInit();

        // Flips the abort flag when the first layer's shader source is read.
        // That read happens inside the first object's dispatch -- past Parse's
        // entry checkpoint and past the first object's own poll -- so the flag
        // is seen by the *second* object's poll, which is exactly the window
        // the real screen-removal abort lands in.
        class AbortOnReadFs : public MemFs {
        public:
            AbortOnReadFs(std::atomic_bool& flag, std::string trigger)
                : m_flag(flag), m_trigger(std::move(trigger)) {}
            std::shared_ptr<fs::IBinaryStream> Open(std::string_view path) override {
                if (path.find(m_trigger) != std::string_view::npos) {
                    ++m_hits;
                    m_flag.store(true, std::memory_order_release);
                }
                return MemFs::Open(path);
            }
            int hits() const { return m_hits; }

        private:
            std::atomic_bool& m_flag;
            std::string       m_trigger;
            int               m_hits { 0 };
        };

        constexpr const char* kFirstMaterialJson = R"({
    "passes": [{ "shader": "first", "blending": "translucent", "textures": [] }]
})";
        constexpr const char* kFirstImageJson    = R"({
    "material": "materials/first.json", "width": 256, "height": 256
})";

        std::atomic_bool abort_flag { false };
        auto             fs_owned = std::make_unique<AbortOnReadFs>(abort_flag, "/shaders/first.");
        auto*            trigger_fs = fs_owned.get();
        fs_owned->add("/shaders/first.vert", kTrivialVert);
        fs_owned->add("/shaders/first.frag", kTrivialFrag);
        fs_owned->add("/models/first.json", kFirstImageJson);
        fs_owned->add("/materials/first.json", kFirstMaterialJson);
        fs_owned->add("/shaders/_t.vert", kTrivialVert);
        fs_owned->add("/shaders/_t.frag", kTrivialFrag);
        fs_owned->add("/models/_plain.json", kPlainImageJson);
        fs_owned->add("/materials/_plain.json", kPlainMaterialJson);

        fs::VFS vfs;
        REQUIRE(vfs.Mount("/assets", std::move(fs_owned)));

        const char*         kSceneJson = R"JSON(
{
  "camera": { "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 300, "name": "first_layer", "image": "models/first.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true },
    { "id": 301, "name": "second_layer", "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        parser.SetAbortFlag(&abort_flag);

        auto scene = parser.Parse("aborted_mid_dispatch", kSceneJson, vfs, sm, props);

        // The flag really did flip from inside the parse, not before it.
        REQUIRE(trigger_fs->hits() >= 1);
        CHECK(parser.IsAborted() == true);
        CHECK(scene == nullptr);
    }

    TEST_CASE("Parse with cleared abort flag completes normally") {
        // Defending against the bricked-subsequent-load regression: even after
        // a prior abort, calling Parse with a cleared flag must succeed.
        auto                vfs = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};

        std::atomic_bool abort_flag { false };
        WPSceneParser    parser;
        parser.SetAbortFlag(&abort_flag);

        auto scene = parser.Parse("clean_run", kFixtureSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        CHECK(scene->scene_id == "clean_run");
        CHECK(parser.IsAborted() == false);
    }

    TEST_CASE("null abort flag leaves the parser uncancellable (back-compat)") {
        auto                vfs = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};

        WPSceneParser parser;
        // No SetAbortFlag call -- default state is nullptr.
        auto scene = parser.Parse("null_flag", kFixtureSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        CHECK(parser.IsAborted() == false);
    }

    TEST_CASE("toggling SetAbortFlag rebinds the polled pointer") {
        // MainHandler ctor binds the parser's flag exactly once at construction;
        // this test confirms a later SetAbortFlag call rebinds to a new pointer.
        std::atomic_bool first { false };
        std::atomic_bool second { true };
        WPSceneParser    parser;
        parser.SetAbortFlag(&first);
        CHECK(parser.IsAborted() == false);
        parser.SetAbortFlag(&second);
        CHECK(parser.IsAborted() == true);
        parser.SetAbortFlag(nullptr);
        CHECK(parser.IsAborted() == false);
    }
}

TEST_SUITE("regression: minimised fuzz crashes") {
    TEST_CASE("regression: minimised fuzz crashes round-trip cleanly") {
        namespace fs2 = std::filesystem;
        const fs2::path dir =
            wallpaper::test::test_data_root() / "fuzz_regressions" / "WPSceneParser";
        if (! fs2::exists(dir)) return;
        for (auto& entry : fs2::directory_iterator(dir)) {
            if (entry.path().extension() != ".bin") continue;
            SUBCASE(entry.path().filename().string().c_str()) {
                std::ifstream in(entry.path(), std::ios::binary);
                std::string   buf(std::istreambuf_iterator<char>(in), {});
                auto          j = nlohmann::json::parse(buf, nullptr, false);
                if (j.is_discarded()) return;
                wallpaper::wpscene::WPScene sc;
                CHECK_NOTHROW([&] {
                    try {
                        sc.FromJson(j);
                    } catch (...) {
                        // Mirrors fuzz_WPSceneParser.cpp's no-throw envelope.
                    }
                }());
            }
        }
    }
}
