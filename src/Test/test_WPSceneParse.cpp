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
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneNode.h"
#include "SpecTexs.hpp"
#include "SystemFontFallback.hpp"
#include "WPUserProperties.hpp"
#include "wpscene/WPModelObject.h"

#include "Audio/SoundManager.h"
#include "Utils/Logging.h"

#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"
#include "Fs/PhysicalFs.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
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

// Collects log bodies for the duration of a scope.  Same pattern as
// test_WPMdlParserFormats.cpp's LogCapture — some parser decisions (e.g. a
// camera silently failing to attach) are otherwise invisible: they don't
// change the return value, only what gets logged.
struct LogCapture {
    LogCapture() {
        lines().clear();
        wallpaper_log_test::setSink(&append);
    }
    ~LogCapture() { wallpaper_log_test::setSink(nullptr); }

    LogCapture(const LogCapture&)            = delete;
    LogCapture& operator=(const LogCapture&) = delete;

    static std::vector<std::string>& lines() {
        static std::vector<std::string> v;
        return v;
    }
    static void append(int, const char* msg) { lines().emplace_back(msg); }

    static bool saw(std::string_view needle) {
        for (const auto& l : lines()) {
            if (l.find(needle) != std::string_view::npos) return true;
        }
        return false;
    }
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

// A translucent material that explicitly asks for depthwrite:enabled -- the
// exact shape hud1.json (Live Solar System, workshop 3662790108) uses. Every
// other material fixture in this file omits depthwrite and gets WPMaterial's
// class default ("disabled"), so only this one can tell LoadMaterial's own
// override apart from a value that was already off.
constexpr const char* kTranslucentDepthwriteMaterialJson = R"({
    "passes": [{
        "shader": "_t",
        "blending": "translucent",
        "depthtest": "enabled",
        "depthwrite": "enabled",
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

// Little-endian byte builder for the .mdl fixtures below.  Same shape as the
// `Bytes` helper in test_WPMdlParserFormats.cpp, returned as a std::string so
// it drops straight into MemFs::add.
struct MdlBytes {
    std::string data;
    void        u8(uint8_t v) { data.push_back(static_cast<char>(v)); }
    void        u16(uint16_t v) {
        u8(v & 0xff);
        u8((v >> 8) & 0xff);
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) u8((v >> (i * 8)) & 0xff);
    }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }
    // NUL-terminated string; also how the MDLV/MDLS/MDAT/MDLA tags are stored.
    void str(std::string_view s) {
        for (char c : s) data.push_back(c);
        u8(0);
    }
    // Column-major 4x4: identity rotation/scale, translation (tx,ty,tz).
    void translated_mat4(float tx, float ty, float tz) {
        const float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, tx, ty, tz, 1 };
        for (float v : m) f32(v);
    }
};

// A flag-9 (pos + uv) model with one submesh, three vertices and one
// triangle, referencing matPath (defaults to the shared trivial material).
std::string makeTriangleModelMdl(std::string_view matPath = "materials/_plain.json") {
    MdlBytes b;
    b.str("MDLV0013");
    b.i32(9); // mdl_flag: pos(3) + texcoord(2)
    b.i32(1);
    b.u32(1); // submesh_count
    b.str(matPath);
    b.i32(0);
    b.u32(3 * 5 * 4); // vertex bytes
    for (int i = 0; i < 3; i++) {
        b.f32((float)i);
        b.f32(0.0f);
        b.f32(0.0f);
        b.f32(0.0f);
        b.f32(0.0f);
    }
    b.u32(6); // one u16 triangle
    b.u16(0);
    b.u16(1);
    b.u16(2);
    return b.data;
}

// A puppet with one root bone at (3,4,5) and one MDAT attachment "tip" on
// that bone at (7,8,9); three skinned vertices and one triangle.
std::string makeOneBonePuppetMdl() {
    MdlBytes b;
    b.str("MDLV0013");
    b.i32(0); // not a model flag → puppet branch
    b.i32(1);
    b.i32(1);
    b.str("materials/_plain.json");
    b.i32(0);
    b.u32(0x01800009u); // standard vertex-size herald
    b.u32(3 * 52);
    for (int i = 0; i < 3; i++) {
        b.f32((float)i);
        b.f32(0.0f);
        b.f32(0.0f);                          // pos
        for (int k = 0; k < 4; k++) b.u32(0); // blend indices
        b.f32(1.0f);
        b.f32(0.0f);
        b.f32(0.0f);
        b.f32(0.0f); // weights
        b.f32(0.0f);
        b.f32(0.0f); // uv
    }
    b.u32(6);
    b.u16(0);
    b.u16(1);
    b.u16(2);
    b.str("MDLS0002");
    b.u32(0); // bones_file_end
    b.u16(1); // bones_num
    b.u16(0);
    b.str("root");
    b.i32(0);
    b.u32(0xFFFFFFFFu);
    b.u32(64);
    b.translated_mat4(3.0f, 4.0f, 5.0f);
    b.str("");
    // MDLS2 extras, all absent.
    b.i16(0);
    b.u8(0);
    b.u32(0);
    b.u32(0);
    b.u8(0);
    b.u8(0);
    b.str("MDAT0001");
    b.u32(0);
    b.u16(1); // one attachment
    b.u16(0); // bone_index
    b.str("tip");
    b.translated_mat4(7.0f, 8.0f, 9.0f);
    b.str("MDLA0000");
    return b.data;
}

// Fragment shader that declares the two uniforms WE's `flat` shader reads, so
// the parser's g_Color / g_Alpha base values are kept on the material.
constexpr const char* kFlatLikeFrag = R"GLSL(
uniform float g_Alpha;
uniform vec3 g_Color;
void main() {
    gl_FragColor = vec4(g_Color, g_Alpha);
}
)GLSL";

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

    TEST_CASE("ParseLightObj honors JSON `visible:false`") {
        const char*         kJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 640, "height": 480 } },
  "objects": [
    { "id": 501, "name": "sun_light",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "light": "point", "color": "1 1 1",
      "radius": 1000.0, "intensity": 1.0,
      "visible": false }
  ]
}
)JSON";
        auto                vfs   = makeEmptyAssetsVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_invisible_light", kJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 1);
        auto* lnode = scene->lights.front()->node();
        REQUIRE(lnode != nullptr);
        CHECK_FALSE(lnode->IsVisible());
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
    // boundary — none reaches ParseImageObj.  These close the two
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
    //  - (b-3) no-effect compose layer nothing depends on — drawn onto
    //          _rt_default, no offscreen RT (3662790108 dock plate).
    //  - (b-4) no-effect compose layer that is both a dependency and
    //          script-referenced — still routed offscreen with its RT.
    //
    // The fixture VFS chain is exactly what runtime sees:
    //   scene.json -> image descriptor -> material -> shader pair.
    // All of them share the makeAssetsVfsWith(...) scaffolding plus
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

    TEST_CASE("E2E: a compose layer is recorded as such, even though it never gets an "
              "imgIdToSourceTexture entry") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/models/util/composelayer.json", kPlainImageJson },
            { "/materials/util/effectpassthrough.json", kPlainMaterialJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 501, "name": "compose_layer",
      "image": "models/util/composelayer.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_compose_marker", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        CHECK(scene->composeLayerIds.count(501) == 1u);
        // Unchanged behavior: still no static-source entry for a compose layer.
        CHECK(scene->imgIdToSourceTexture.count(501) == 0u);
    }

    TEST_CASE("E2E: a project layer's effect-chain pingpong binds to the render output, "
              "not a fixed size") {
        ensureGlslangInit();
        // models/util/projectlayer.json, mirrored verbatim from the shipped asset
        // (~/.local/share/Steam/steamapps/common/wallpaper_engine/assets/models/util/
        // projectlayer.json): passthrough + autosize + projectlayer, no "fullscreen"
        // key.  No width/height either, so the scene-object's own "size" wins
        // (WPImageObject.cpp:474-482) instead of autosize's texture-probe path.
        auto vfs = makeAssetsVfsWith({
            { "/models/util/projectlayer.json",
              R"({"material":"materials/_plain.json","passthrough":true,)"
              R"("autosize":true,"projectlayer":true})" },
            { "/effects/tint.json", kEffectFileJson },
        });

        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 3840, "height": 2160 } },
  "objects": [
    { "id": 501, "name": "full_composition",
      "image": "models/util/projectlayer.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "3840 2160",
      "visible": true,
      "effects": [ { "id": 10, "name": "tint", "visible": true,
                     "file": "effects/tint.json" } ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_project_layer", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(501) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(501);
        REQUIRE(effLayer != nullptr);

        REQUIRE(scene->renderTargets.count(effLayer->FirstTarget()) == 1);
        const auto& rt = scene->renderTargets.at(effLayer->FirstTarget());
        // A project layer recaptures the whole composited frame every tick, so
        // its pingpong must track the render output extent exactly like a
        // fullscreen layer -- otherwise RenderScale shrinks _rt_default while
        // this pingpong stays pinned at its authored size, wasting the whole
        // chain on an upsampled capture.
        CHECK(rt.bind.enable == true);
        CHECK(rt.bind.screen == true);
    }

    TEST_CASE("E2E: a canvas-sized non-project image layer's effect-chain pingpong "
              "keeps its authored size") {
        // Regression guard: a plain, texture-native layer whose authored size
        // happens to equal the ortho canvas (3705485676's "MAIN", a real
        // painted 3840x2160 background) must NOT be swept into the
        // project-layer fix just because its size matches the canvas -- WE
        // sizes those at texture-native resolution on purpose, and shrinking
        // them blurs the artist's background.  Pins the fix at the
        // `projectlayer` flag, not at "size == canvas".
        ensureGlslangInit();
        // No width/height in the model JSON (same reasoning as above): the
        // scene-object's "size" must be what drives this object's dimensions,
        // so a widened condition keyed off size would (wrongly) catch it too.
        auto vfs = makeAssetsVfsWith({
            { "/models/plain_canvas_sized.json", R"({"material":"materials/_plain.json"})" },
            { "/effects/tint.json", kEffectFileJson },
        });

        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 3840, "height": 2160 } },
  "objects": [
    { "id": 502, "name": "background",
      "image": "models/plain_canvas_sized.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "3840 2160",
      "visible": true,
      "effects": [ { "id": 10, "name": "tint", "visible": true,
                     "file": "effects/tint.json" } ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_canvas_sized_plain_layer", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(502) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(502);
        REQUIRE(effLayer != nullptr);

        REQUIRE(scene->renderTargets.count(effLayer->FirstTarget()) == 1);
        const auto& rt = scene->renderTargets.at(effLayer->FirstTarget());
        CHECK(rt.bind.enable == false);
        CHECK(rt.bind.screen == false);
    }

    TEST_CASE("E2E: a project layer's per-effect scratch FBO binds to the render output too") {
        ensureGlslangInit();
        // Same shipped models/util/projectlayer.json as the case above, but the
        // effect declares its own scratch FBO -- the shape every real project
        // layer has (3705485676's blurprecise/godrays chains, 2992803622's
        // bokeh_blur/bloom).  Those per-effect FBOs are sized on their own gate
        // (WPSceneParser.cpp:2284-2298), separate from the pingpong, so a layer
        // whose chain declares "fbos" needs the flag to reach both.
        auto vfs = makeAssetsVfsWith({
            { "/models/util/projectlayer.json",
              R"({"material":"materials/_plain.json","passthrough":true,)"
              R"("autosize":true,"projectlayer":true})" },
            { "/effects/trail.json", kEffectWithCopyCommandJson },
        });

        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 3840, "height": 2160 } },
  "objects": [
    { "id": 503, "name": "full_composition",
      "image": "models/util/projectlayer.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "3840 2160",
      "visible": true,
      "effects": [ { "id": 10, "name": "trail", "visible": true,
                     "file": "effects/trail.json" } ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_project_layer_effect_fbo", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(503) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(503);
        REQUIRE(effLayer != nullptr);

        // Find the authored effect by name: a compose layer can carry a
        // synthesized passthrough ahead of it, so index 0 is not guaranteed.
        std::shared_ptr<SceneImageEffect> trail;
        for (std::size_t i = 0; i < effLayer->EffectCount(); i++) {
            const auto& e = effLayer->GetEffect(i);
            if (e && e->name == "trail") trail = e;
        }
        REQUIRE(trail != nullptr);

        // The effect's copy command targets its scratch FBO, so the command's
        // destination names the render target the "fbos" entry produced.
        REQUIRE(trail->commands.size() == 1);
        const std::string& fboName = trail->commands.at(0).dst;
        REQUIRE(fboName.find("FullCompoBuffer1") != std::string::npos);
        REQUIRE(scene->renderTargets.count(fboName) == 1);

        const auto& fbo = scene->renderTargets.at(fboName);
        CHECK(fbo.bind.enable == true);
        CHECK(fbo.bind.screen == true);
        // Screen-bound FBOs are scaled by the reciprocal of the "fbos" entry's
        // own scale; this one asks for full resolution.
        CHECK(fbo.bind.scale == doctest::Approx(1.0));
        // A screen-bound RT carries a placeholder extent the renderer rewrites
        // from the swapchain each resize (VulkanRender.cpp:1592-1594).  Unbound
        // it stays frozen at the object's authored 3840x2160 no matter what the
        // render output is doing.
        CHECK(fbo.width != 3840);
        CHECK(fbo.height != 2160);
    }

    TEST_CASE("E2E: no-effect compose layer with no dependent stays visible, not offscreen "
              "(b-3)") {
        ensureGlslangInit();
        // Same compose-layer marker + synthesized-passthrough material as
        // b-1, but this scene has exactly one object: nothing can name id
        // 501 in a `dependencies` array, so context.compose_dependency_ids
        // stays empty and the synthesized passthrough must NOT be forced
        // offscreen (Live Solar System 3662790108 id=1359: the dock's
        // background plate, with no dependent, silently never drew).
        auto vfs = makeAssetsVfsWith({
            { "/models/util/composelayer.json", kPlainImageJson },
            { "/materials/util/effectpassthrough.json", kPlainMaterialJson },
        });

        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 501, "name": "lone_compose_layer",
      "image": "models/util/composelayer.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_compose_no_dependent", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        // synthesizePassthroughForCompose still fires (no authored effects,
        // image == composelayer.json) -- the layer gets an effect chain
        // either way.  The bug forced that chain's output offscreen even
        // though nothing reads it.
        REQUIRE(scene->nodeEffectLayerMap.count(501) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(501);
        REQUIRE(effLayer != nullptr);
        CHECK(effLayer->IsOffscreen() == false);
        CHECK(scene->renderTargets.count(GenOffscreenRT(501)) == 0);
    }

    TEST_CASE("E2E: a script-referenced no-effect compose layer with a dependent still "
              "routes offscreen (b-4)") {
        ensureGlslangInit();
        // The counterpart to b-3: id 601 IS named in another compose layer's
        // `dependencies`, so its synthesized passthrough has to write the
        // isolated sprite RT that dependent samples through
        // _rt_imageLayerComposite_601_a.
        //
        // Being script-referenced (a getLayer('compose_source') anywhere in the
        // scene's scripts) is what makes this case worth its own test:
        // computeOffscreenRouting deliberately leaves scripted layers in the
        // main render graph, so the generic "referenced as compose dependency"
        // rule there does NOT fire and the node itself stays on-screen.  The
        // offscreen route for the effect chain can only come from the
        // compose-dependency check in ParseImageObj.
        auto vfs = makeAssetsVfsWith({
            { "/models/util/composelayer.json", kPlainImageJson },
            { "/materials/util/effectpassthrough.json", kPlainMaterialJson },
        });

        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 600, "name": "script_host",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": {
        "script": "function update(v){ return thisScene.getLayer('compose_source').visible; }",
        "scriptproperties": {},
        "value": true
      } },
    { "id": 601, "name": "compose_source",
      "image": "models/util/composelayer.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true },
    { "id": 602, "name": "compose_consumer",
      "image": "models/util/composelayer.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "dependencies": [601] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_compose_scripted_dep", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren()) {
                if (auto* hit = findById(c.get(), id)) return hit;
            }
            return nullptr;
        };
        SceneNode* source = findById(scene->sceneGraph.get(), 601);
        REQUIRE(source != nullptr);
        // The script reference kept the node itself in the main graph...
        CHECK(source->IsOffscreen() == false);

        // ...while its synthesized passthrough writes the sprite RT its
        // dependent reads, rather than blending onto _rt_default.
        REQUIRE(scene->nodeEffectLayerMap.count(601) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(601);
        REQUIRE(effLayer != nullptr);
        CHECK(effLayer->IsOffscreen() == true);
        CHECK(scene->renderTargets.count(GenOffscreenRT(601)) == 1);
    }

    TEST_CASE("E2E: a 3D scene's compose layer camera mirrors the ortho overlay, not "
              "an unattached node") {
        ensureGlslangInit();
        // A 3D (perspective) scene's active camera is a direct-lookat camera
        // with no scene-graph node — SetDirectLookAt never attaches one.
        // Before this fix, a compose layer's camera unconditionally shared
        // that node; AttatchNode(nullptr) logs and bails out before Update(),
        // so the camera's view-projection matrix stayed at Identity and the
        // compose layer's effect chain drew through no projection at all.  A
        // flat compose layer composites through the ortho overlay camera
        // ("global_ortho") exactly like every other flat layer in a 3D scene,
        // so its own camera should mirror THAT one instead of "global".
        auto vfs = makeAssetsVfsWith({
            { "/models/util/composelayer.json", kPlainImageJson },
            { "/materials/util/effectpassthrough.json", kPlainMaterialJson },
        });

        // A "camera" block is required for WPScene::FromJson to even call
        // general.FromJson (its absence short-circuits with "scene no
        // camera" and general.isOrtho is left at its struct default `true`).
        // With "camera" present and no "orthogonalprojection" key,
        // general.isOrtho ends up false -> the parser builds a perspective
        // "global" + ortho-overlay "global_ortho".
        const char* kSceneJson = R"JSON(
{
  "camera": { "eye": "0 0 1000", "center": "0 0 0", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0" },
  "objects": [
    { "id": 601, "name": "compose_layer_3d",
      "image": "models/util/composelayer.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;

        LogCapture log;
        auto scene = parser.Parse("scene_compose_3d", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        CHECK_FALSE(LogCapture::saw("Attach a null node to camera"));

        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren()) {
                if (auto* hit = findById(c.get(), id)) return hit;
            }
            return nullptr;
        };
        SceneNode* compose = findById(scene->sceneGraph.get(), 601);
        REQUIRE(compose != nullptr);

        const std::string camName = compose->Camera();
        REQUIRE(scene->cameras.count(camName) == 1);
        REQUIRE(scene->cameras.count("global_ortho") == 1);

        // Follows "global_ortho" — "global" in a 3D scene is the perspective
        // camera, which UpdateCameraFillMode never resizes a linked follower
        // against.
        REQUIRE(scene->linkedCameras.count("global_ortho") == 1);
        const auto& orthoFollowers = scene->linkedCameras.at("global_ortho");
        CHECK(std::find(orthoFollowers.begin(), orthoFollowers.end(), camName) !=
              orthoFollowers.end());
        if (scene->linkedCameras.count("global") == 1) {
            const auto& globalFollowers = scene->linkedCameras.at("global");
            CHECK(std::find(globalFollowers.begin(), globalFollowers.end(), camName) ==
                  globalFollowers.end());
        }

        // The compose camera shares the ortho overlay's node — not merely a
        // clone of it — so a later camera-shake/parallax nudge to that node
        // reaches the compose camera too.  Its view matrix (the node-derived
        // part) must therefore match exactly; only the projection (near/far/
        // size, sized to the compose effect's own bounds, same as it always
        // was for the 2D "global" case) is free to differ.
        auto composeCam = scene->cameras.at(camName);
        auto orthoCam   = scene->cameras.at("global_ortho");
        REQUIRE(composeCam->GetAttachedNode() != nullptr);
        CHECK(composeCam->GetAttachedNode() == orthoCam->GetAttachedNode());

        auto view      = composeCam->GetViewMatrix();
        auto orthoView = orthoCam->GetViewMatrix();
        CHECK(view.isApprox(orthoView));
        // Pre-fix this stayed at the Eigen default -- Identity -- because
        // AttatchNode(nullptr) bailed out before ever calling Update().
        CHECK_FALSE(composeCam->GetViewProjectionMatrix().isApprox(Eigen::Matrix4d::Identity()));

        // This scene's JSON never sets "orthogonalprojection", so
        // general's struct default (1920x1080, WPScene.h) flows straight
        // through to the compose camera's own projection size -- confirms
        // the `context.ortho_w > 0` branch of the size fallback actually
        // ran, not the `scene.activeCamera->Width()` fallback (which would
        // read back 1.0, the perspective camera's untouched default).
        CHECK(composeCam->Width() == doctest::Approx(1920));
        CHECK(composeCam->Height() == doctest::Approx(1080));
    }

    TEST_CASE("E2E: a flat layer with an effect in a 3D scene composites through the ortho "
              "overlay") {
        ensureGlslangInit();
        // Same "camera"-block-without-orthogonalprojection shape as the
        // compose-layer case above, but a plain (non-compose) image with its
        // own effect chain — assembleEffectChain's own SetFinalCamera call
        // (WPSceneParser.cpp, just after assembleEffects), not the compose
        // "mirror whichever camera" branch.
        auto vfs = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
        });

        const char*         kSceneJson = R"JSON(
{
  "camera": { "eye": "0 0 1000", "center": "0 0 0", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0" },
  "objects": [
    { "id": 701, "name": "flat_with_effect",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
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
        auto scene = parser.Parse("scene_flat_effect_ortho_final_cam", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->cameras.count("global_ortho") == 1);

        REQUIRE(scene->nodeEffectLayerMap.count(701) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(701);
        REQUIRE(effLayer != nullptr);

        // The layer is flat (no "perspective" flag, no model parent), so its
        // final composite must go through the ortho overlay, not the scene's
        // perspective "global".
        CHECK(effLayer->FinalCamera() == "global_ortho");
    }

    TEST_CASE("E2E: a flat layer without effects in a 3D scene uses the ortho overlay camera") {
        ensureGlslangInit();
        // Mirrors the effect case above but through applyFlatPerspectiveOrthoCamera's
        // ! hasEffect branch, which sets the camera directly on the worldNode
        // instead of on a SceneImageEffectLayer's final camera.
        auto vfs = makeAssetsVfsWith({});

        const char*         kSceneJson = R"JSON(
{
  "camera": { "eye": "0 0 1000", "center": "0 0 0", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0" },
  "objects": [
    { "id": 702, "name": "flat_no_effect",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_flat_noeffect_ortho_cam", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->cameras.count("global_ortho") == 1);

        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren()) {
                if (auto* hit = findById(c.get(), id)) return hit;
            }
            return nullptr;
        };
        SceneNode* node = findById(scene->sceneGraph.get(), 702);
        REQUIRE(node != nullptr);

        // No effect chain, so the camera is set directly on the world node
        // rather than on a SceneImageEffectLayer's final camera.
        CHECK(node->Camera() == "global_ortho");
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

    // adjustBlendForTextEffects's own hasEffect gate turns out to already agree
    // with assembleEffects's independent computeWouldBuildEffects call for
    // every effects-array shape (both reduce to "does some effect resolve
    // visible==true"; see the comment above computeWouldBuildEffects). What
    // this pins down is the piece that previously wasn't shared: a text
    // layer's bound-but-currently-hidden effect ahead of a visible one still
    // gets built (not dropped) so a later toggle can show it without a
    // reparse -- same as an image layer already does.
    TEST_CASE("E2E: a text layer's bound-and-hidden effect still builds "
              "alongside its visible one, same as an image layer") {
        ensureGlslangInit();
        if (ResolveSystemFontFallback("systemfont_sans").empty()) return;

        auto                vfs        = makeAssetsVfsWith({
            { "/effects/glow.json", kEffectFileJson },
            { "/effects/trail.json", kEffectWithCopyCommandJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 502, "name": "toggle_text",
      "origin": "100 200 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "256 64",
      "font": "systemfont_sans", "pointsize": 24,
      "text": { "value": "12:00" },
      "visible": true,
      "effects": [
        { "id": 12, "name": "glow",
          "visible": {"user": "glow_enable", "value": true},
          "file": "effects/glow.json" },
        { "id": 13, "name": "trail", "visible": true, "file": "effects/trail.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props;
        REQUIRE(props.LoadFromProjectJson(
            R"({"general":{"properties":{"glow_enable":{"value":false,"type":"bool"}}}})"));
        WPSceneParser parser;
        auto          scene = parser.Parse("scene_text_mixed_effects", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(502) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(502);
        // Both effects built: "trail" as the visible terminal, "glow" ahead
        // of it hidden rather than dropped.
        REQUIRE(effLayer->EffectCount() == 2u);

        // Find by visibility rather than name: the effect's built name comes
        // from its effect-file content (kEffectFileJson's own "name": "tint"),
        // not the scene JSON's per-effect "name" key.
        std::shared_ptr<SceneImageEffect> hidden;
        for (std::size_t i = 0; i < effLayer->EffectCount(); i++) {
            if (! effLayer->GetEffect(i)->visible) hidden = effLayer->GetEffect(i);
        }
        REQUIRE(hidden != nullptr);

        REQUIRE(scene->userPropEffectVisBindings.count("glow_enable") == 1u);
    }

    TEST_CASE("E2E: text layer reading a composite RT by raw texture string forces its source "
              "offscreen") {
        ensureGlslangInit();
        if (ResolveSystemFontFallback("systemfont_sans").empty()) return;

        // No "dependencies" entry anywhere in this scene -- 401 is only ever
        // referenced by name, inside 501's effect material's texture list.
        const char* kCompositeReaderMaterialJson = R"({
            "passes": [{
                "shader": "_t",
                "blending": "translucent",
                "textures": ["_rt_imageLayerComposite_401_a"]
            }]
        })";
        const char* kCompositeReaderEffectJson   = R"({
            "name": "compositeReader",
            "passes": [{
                "material": "materials/_compositeReader.json"
            }]
        })";

        auto vfs = makeAssetsVfsWith({
            { "/effects/compositeReader.json", kCompositeReaderEffectJson },
            { "/materials/_compositeReader.json", kCompositeReaderMaterialJson },
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
    { "id": 501, "name": "clock_text",
      "origin": "100 200 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "256 64",
      "font": "systemfont_sans", "pointsize": 24,
      "text": { "value": "12:00" },
      "visible": true,
      "effects": [
        { "id": 11, "name": "compositeReader", "visible": true,
          "file": "effects/compositeReader.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_text_composite_reader", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren()) {
                if (auto* hit = findById(c.get(), id)) return hit;
            }
            return nullptr;
        };
        SceneNode* dep = findById(scene->sceneGraph.get(), 401);
        REQUIRE(dep != nullptr);

        // Before this fix: prescanDependencies never looked at any text object's
        // effect materials, so 401 stayed onscreen and 501's compositeReader
        // effect sampled a full-FB snapshot at 401's own draw position instead
        // of an isolated sprite.
        CHECK(dep->IsOffscreen() == true);
        CHECK(scene->renderTargets.count(GenOffscreenRT(401)) == 1);
    }

    // A solid layer is a coloured quad: WE draws it with the object's own
    // `color` and `alpha`.  Authors who want an invisible pivot set alpha 0
    // themselves (Rei Ayanami 3061226599 does exactly that on its "Solid"
    // group anchors, and paints its whole background with a blue one).
    // Forcing every solidlayer transparent erased that background.
    TEST_CASE("E2E: a solid layer keeps its authored colour and alpha") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/shaders/_flat.frag", kFlatLikeFrag },
            { "/shaders/_flat.vert", kTrivialVert },
            { "/materials/_flat.json",
              R"({ "passes": [{ "shader": "_flat", "blending": "translucent", "textures": [] }] })" },
            { "/models/_solid.json",
              R"({ "material": "materials/_flat.json", "solidlayer": true })" },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 310, "name": "background", "image": "models/_solid.json",
      "origin": "640 360 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "100 100", "color": "0.1 0.4 0.6", "alpha": 0.75,
      "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_solid_layer", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        auto it = scene->nodeById.find(310);
        REQUIRE(it != scene->nodeById.end());
        REQUIRE(it->second->Mesh() != nullptr);
        REQUIRE(it->second->Mesh()->Material() != nullptr);
        const auto& cv = it->second->Mesh()->Material()->customShader.constValues;
        REQUIRE(cv.count("g_Alpha") == 1);
        CHECK(cv.at("g_Alpha")[0] == doctest::Approx(0.75f));
        REQUIRE(cv.count("g_Color") == 1);
        CHECK(cv.at("g_Color")[0] == doctest::Approx(0.1f));
        CHECK(cv.at("g_Color")[1] == doctest::Approx(0.4f));
        CHECK(cv.at("g_Color")[2] == doctest::Approx(0.6f));
    }

    TEST_CASE("E2E: a translucent 2D image layer forces depthWrite off even when its material "
              "JSON asks for depthwrite:enabled") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/models/_hud.json", R"({ "material": "materials/_hud_dw.json" })" },
            { "/materials/_hud_dw.json", kTranslucentDepthwriteMaterialJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 701, "name": "hud", "image": "models/_hud.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "256 256", "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_translucent_depthwrite_image", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        auto it = scene->nodeById.find(701);
        REQUIRE(it != scene->nodeById.end());
        REQUIRE(it->second->Mesh() != nullptr);
        REQUIRE(it->second->Mesh()->Material() != nullptr);
        // A translucent 2D layer whose near-transparent pixels still write
        // real depth blocks every later draw from painting that screen
        // rectangle for the rest of the frame -- the "hud1" overlay on
        // workshop 3662790108 does exactly this. depthTest may stay on;
        // depthWrite must not.
        CHECK(it->second->Mesh()->Material()->depthTest);
        CHECK_FALSE(it->second->Mesh()->Material()->depthWrite);
    }

    // Model objects take the same parent-chain rules as image children.  A
    // parent with its own effect has its world node reset to identity for the
    // base capture, so a model chained through it lands at its bare local
    // offset — Rei Ayanami 3061226599's "Stone 1" rock, parented to the
    // effect-bearing puppet, fell to the scene corner and dragged its
    // compose-layer child with it.
    TEST_CASE("E2E: model child of an effect parent inherits the parent world") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
            { "/models/_tri.mdl", makeTriangleModelMdl() },
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
    { "id": 501, "name": "rock", "parent": 401,
      "model": "models/_tri.mdl",
      "origin": "10 20 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_model_child_fx_parent", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);

        auto cit = scene->nodeById.find(501);
        REQUIRE(cit != scene->nodeById.end());
        SceneNode* rock = cit->second;
        REQUIRE(rock != nullptr);
        rock->UpdateTrans();
        const auto& m = rock->ModelTrans();
        CHECK(m(0, 3) == doctest::Approx(110.0));
        CHECK(m(1, 3) == doctest::Approx(70.0));
    }

    TEST_CASE("E2E: a translucent 3D model submesh still forces depthWrite off when its material "
              "JSON asks for depthwrite:enabled") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/materials/_tri_dw.json", kTranslucentDepthwriteMaterialJson },
            { "/models/_tri_dw.mdl", makeTriangleModelMdl("materials/_tri_dw.json") },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 801, "name": "rock_dw", "model": "models/_tri_dw.mdl",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_model_translucent_depthwrite", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        auto it = scene->nodeById.find(801);
        REQUIRE(it != scene->nodeById.end());
        // ParseModelObj's node holds the transform; the submesh (and its
        // Mesh/Material) live on a child node, not the parent registered in
        // nodeById -- a single-submesh model has exactly one child.
        REQUIRE(! it->second->GetChildren().empty());
        SceneNode* submesh = it->second->GetChildren().front().get();
        REQUIRE(submesh->Mesh() != nullptr);
        REQUIRE(submesh->Mesh()->Material() != nullptr);
        CHECK(submesh->Mesh()->Material()->depthTest);
        CHECK_FALSE(submesh->Mesh()->Material()->depthWrite);
    }

    // A model can rig to a named attachment on the parent puppet, like an
    // image child.  Its world then includes the bone's rest world and the
    // attachment matrix: parent(100,50) * bone(3,4) * tip(7,8) * local(10,20).
    TEST_CASE("E2E: model rigged to a parent puppet attachment lands on the bone") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/models/_pup.json",
              R"({ "material": "materials/_plain.json", "puppet": "models/_pup.mdl" })" },
            { "/models/_pup.mdl", makeOneBonePuppetMdl() },
            { "/models/_tri.mdl", makeTriangleModelMdl() },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 601, "name": "puppet", "image": "models/_pup.json",
      "origin": "100 50 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "256 256", "visible": true },
    { "id": 602, "name": "rock", "parent": 601, "attachment": "tip",
      "model": "models/_tri.mdl",
      "origin": "10 20 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_model_on_bone", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->nodePuppetMap.count(601) == 1);

        auto cit = scene->nodeById.find(602);
        REQUIRE(cit != scene->nodeById.end());
        SceneNode* rock = cit->second;
        REQUIRE(rock != nullptr);
        rock->UpdateTrans();
        const auto& m = rock->ModelTrans();
        CHECK(m(0, 3) == doctest::Approx(120.0));
        CHECK(m(1, 3) == doctest::Approx(82.0));
    }

    TEST_CASE("WPModelObject reads the attachment name") {
        auto                   vfs = makeEmptyAssetsVfs();
        auto                   j   = nlohmann::json::parse(R"({
            "id": 7, "name": "rock", "model": "models/x.mdl", "parent": 3,
            "attachment": "Stone", "origin": "1 2 3" })");
        wpscene::WPModelObject obj;
        REQUIRE(obj.FromJson(j, *vfs));
        CHECK(obj.attachment == "Stone");
        CHECK(obj.parent_id == 3);
    }

    TEST_CASE("E2E: HDR bloom builds the fixed four-mip, eight-pass chain") {
        ensureGlslangInit();
        auto vfs = makeAssetsVfsWith({
            { "/shaders/hdr_downsample.vert", kTrivialVert },
            { "/shaders/hdr_downsample.frag", kTrivialFrag },
            { "/shaders/combine_hdr.vert", kTrivialVert },
            { "/shaders/combine_hdr.frag", kTrivialFrag },
        });

        const char*         kSceneJson = R"JSON(
{
  "camera": { "eye": "0 0 1000", "center": "0 0 0", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1920, "height": 1080,
                                         "postprocessing": "ultra" },
               "hdr": true, "bloom": true, "bloomhdrfeather": 0.2 },
  "objects": []
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_bloom_fixed_chain", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->bloomConfig.enabled);

        // 1 extract + 3 down + 3 up + 1 compose = 8, at 4 mip levels.
        REQUIRE(scene->bloomConfig.nodes.size() == 8);
        REQUIRE(scene->bloomConfig.outputs.size() == 8);
        CHECK(scene->bloomConfig.outputs[0] == "_rt_Bloom_Mip1");
        CHECK(scene->bloomConfig.outputs.back() == std::string(SpecTex_Default));
        CHECK(scene->renderTargets.count("_rt_Bloom_Mip4") == 1);
        CHECK(scene->renderTargets.count("_rt_Bloom_Mip5") == 0);

        // The knee now tracks the scene's own bloomhdrfeather (WE's default
        // is 0.1; this fixture authors 0.2).  Scatter and chain depth stay
        // hard-coded: the shipped hdr_downsample.frag multiplies by
        // g_BloomScatter at every upsample level, so feeding the authored
        // value through compounds geometrically over the chain and blew up
        // a real wallpaper's bloom in a prior attempt.  Pin what's still
        // hard-coded so a future nudge doesn't restyle every HDR
        // wallpaper's bloom silently.
        const auto& extract = scene->bloomConfig.nodes[0]->Mesh()->Material()->customShader;
        auto        knee    = extract.constValues.find("g_BloomBlendParams");
        REQUIRE(knee != extract.constValues.end());
        REQUIRE(knee->second.size() == 4);
        // .z is 2*knee, which pins knee at 0.20 (this fixture's
        // bloomhdrfeather) without depending on the threshold that .x and
        // .y are offset from.
        CHECK(knee->second[2] == doctest::Approx(0.40f)); // 2 * bloomhdrfeather (0.2)

        // nodes[4] is the first UPSAMPLE=1 pass — 1 extract + 3 downsamples
        // precede it.  It runs the hdr_downsample shader too, so g_BloomScatter
        // rides on the same material as the downsample chain.
        const auto& upsample = scene->bloomConfig.nodes[4]->Mesh()->Material()->customShader;
        auto        scatter  = upsample.constValues.find("g_BloomScatter");
        REQUIRE(scatter != upsample.constValues.end());
        REQUIRE(scatter->second.size() == 1);
        CHECK(scatter->second[0] == doctest::Approx(1.0f));
    }

    TEST_CASE("E2E: an exactly-zero bloomhdrfeather does not divide by zero") {
        ensureGlslangInit();
        auto vfs = makeAssetsVfsWith({
            { "/shaders/hdr_downsample.vert", kTrivialVert },
            { "/shaders/hdr_downsample.frag", kTrivialFrag },
            { "/shaders/combine_hdr.vert", kTrivialVert },
            { "/shaders/combine_hdr.frag", kTrivialFrag },
        });

        // Real installed content authors bloomhdrfeather:0.0 (3453251764,
        // 3562086244 -- both currently ship bloom:false so neither reaches
        // this code path today, but nothing in the schema forbids the
        // combination, and the divide below is unconditional either way).
        const char*         kSceneJson = R"JSON(
{
  "camera": { "eye": "0 0 1000", "center": "0 0 0", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1920, "height": 1080,
                                         "postprocessing": "ultra" },
               "hdr": true, "bloom": true, "bloomhdrfeather": 0.0 },
  "objects": []
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_bloom_zero_feather", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->bloomConfig.enabled);

        // g_BloomBlendParams.w is 0.25/knee, computed here in C++ at parse
        // time -- an unfloored knee of exactly 0 is already +inf before the
        // value ever reaches the shader, so this doesn't need a Vulkan
        // device or a shader run to catch.
        const auto& extract = scene->bloomConfig.nodes[0]->Mesh()->Material()->customShader;
        auto        knee    = extract.constValues.find("g_BloomBlendParams");
        REQUIRE(knee != extract.constValues.end());
        REQUIRE(knee->second.size() == 4);
        for (std::size_t i = 0; i < knee->second.size(); ++i) CHECK(std::isfinite(knee->second[i]));
    }

    TEST_CASE("E2E: HDR bloom's per-pass UV offset stays finite when the scene authors a "
              "zero-width orthographic projection") {
        ensureGlslangInit();
        auto vfs = makeAssetsVfsWith({
            { "/shaders/hdr_downsample.vert", kTrivialVert },
            { "/shaders/hdr_downsample.frag", kTrivialFrag },
            { "/shaders/combine_hdr.vert", kTrivialVert },
            { "/shaders/combine_hdr.frag", kTrivialFrag },
        });

        const char*         kSceneJson = R"JSON(
{
  "camera": { "eye": "0 0 1000", "center": "0 0 0", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 0, "height": 720,
                                         "postprocessing": "ultra" },
               "hdr": true, "bloom": true },
  "objects": []
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_zero_width_bloom", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->bloomConfig.enabled);

        // nodes[0] is the Extract stage (src_scale 1.0), whose g_RenderVar0
        // holds (-dx, -dy, dx, dy).  A zero-width scene makes dx = 0.5/(0*1.0)
        // -- +inf today, before this step's floor.
        const auto& extract = scene->bloomConfig.nodes[0]->Mesh()->Material()->customShader;
        auto        v       = extract.constValues.find("g_RenderVar0");
        REQUIRE(v != extract.constValues.end());
        for (std::size_t i = 0; i < v->second.size(); ++i) CHECK(std::isfinite(v->second[i]));
    }

    // -----------------------------------------------------------------------
    // Mutation-coverage gaps in assembleEffects()/assembleEffectChain()
    // (WPSceneParser.cpp's shared effect-chain assembler for image and text
    // layers).  The file had no mutation baseline before that share landed, so
    // this arithmetic -- correct today -- had no fixture able to tell if it
    // stopped being correct.  Each test below is paired with a hand-applied,
    // hand-reverted mutation on the production line it targets to prove the
    // assertion actually depends on that line.
    // -----------------------------------------------------------------------

    TEST_CASE("kills the FBO div_to_mul at both width and height (non-fullscreen effect "
              "scratch buffer)") {
        // The only existing "fbos" fixture (kEffectWithCopyCommandJson) declares
        // scale:1, where size/scale and size*scale land on the same uint16_t --
        // division and multiplication are indistinguishable there, and that
        // fixture also runs the *fullscreen* branch (a different line) besides.
        // scale:2 on a plain, non-fullscreen layer forces the
        // `.width = base_width / scale` branch and gives division and
        // multiplication genuinely different, non-truncated answers.
        ensureGlslangInit();
        // The model's own "width"/"height" wins over the scene object's "size"
        // field (wpscene/WPImageObject.cpp: `if (jImage.contains("width"))` is
        // checked before `json.contains("size")`), so an 800x400 layer needs a
        // model file authored at that size -- kPlainImageJson is a shared
        // 256x256 and would silently override it.
        constexpr const char* kEffectWithHalfScaleFboJson = R"({
            "name": "halfscale",
            "fbos": [{ "name": "HalfBuffer", "scale": 2 }],
            "passes": [
                { "command": "copy", "target": "HalfBuffer", "source": "previous" },
                { "material": "materials/_plain.json" }
            ]
        })";
        constexpr const char* kHalfScaleSourceImageJson   = R"({
            "material": "materials/_plain.json",
            "width": 800,
            "height": 400
        })";
        auto                  vfs                         = makeAssetsVfsWith({
            { "/effects/halfscale.json", kEffectWithHalfScaleFboJson },
            { "/models/halfscale_source.json", kHalfScaleSourceImageJson },
        });
        const char*           kSceneJson                  = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 504, "name": "halfscale_layer",
      "image": "models/halfscale_source.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [ { "id": 10, "name": "halfscale", "visible": true,
                     "file": "effects/halfscale.json" } ] }
  ]
}
)JSON";
        audio::SoundManager   sm;
        WPUserProperties      props {};
        WPSceneParser         parser;
        auto scene = parser.Parse("scene_halfscale_fbo", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(504) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(504);
        REQUIRE(effLayer != nullptr);

        std::shared_ptr<SceneImageEffect> halfscale;
        for (std::size_t i = 0; i < effLayer->EffectCount(); i++) {
            const auto& e = effLayer->GetEffect(i);
            if (e && e->name == "halfscale") halfscale = e;
        }
        REQUIRE(halfscale != nullptr);
        REQUIRE(halfscale->commands.size() == 1);
        const std::string& fboName = halfscale->commands.at(0).dst;
        REQUIRE(fboName.find("HalfBuffer") != std::string::npos);
        REQUIRE(scene->renderTargets.count(fboName) == 1);

        const auto& fbo = scene->renderTargets.at(fboName);
        // 800/2 x 400/2 = 400x200.  The div_to_mul mutant produces
        // 800*2 x 400*2 = 1600x800 instead -- both fit uint16_t, so nothing
        // truncates the two apart.
        CHECK(fbo.width == 400);
        CHECK(fbo.height == 200);
        // Non-fullscreen chain: not bound to the screen/swapchain.
        CHECK(fbo.bind.enable == false);
    }

    TEST_CASE("lists every effect on a layer, not just the first, in layerEffectNames") {
        // Every existing effects fixture in this file declares exactly one visible
        // effect.  At count==1, `i++` and its mutant `i--` (size_t underflows to
        // SIZE_MAX on the first decrement, and `SIZE_MAX < 1` is false) both run
        // the loop body exactly once -- the mutant is equivalent there.  Two
        // effects is the minimum fixture that can tell the two loop directions
        // apart: `i--` stops after one iteration, `i++` continues to the second.
        ensureGlslangInit();
        constexpr const char* kBloomEffectFileJson = R"({
            "name": "bloom",
            "passes": [{
                "material": "materials/_plain.json"
            }]
        })";
        auto                  vfs                  = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
            { "/effects/bloom.json", kBloomEffectFileJson },
        });
        const char*           kSceneJson           = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 505, "name": "two_effect_layer",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "tint", "visible": true, "file": "effects/tint.json" },
        { "id": 11, "name": "bloom", "visible": true, "file": "effects/bloom.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager   sm;
        WPUserProperties      props {};
        WPSceneParser         parser;
        auto scene = parser.Parse("scene_two_effects", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->layerEffectNames.count("two_effect_layer") == 1);
        CHECK(scene->layerEffectNames.at("two_effect_layer") ==
              std::vector<std::string> { "tint", "bloom" });
    }

    TEST_CASE("a user-bound effect whose CURRENT value resolves false still gets built, just "
              "hidden -- as long as it isn't the last effect on its layer") {
        ensureGlslangInit();
        // WPImageEffect::FromFileJson reads the effect's `name` from the FILE's
        // own JSON, not the scene-level effects[] entry -- so each fixture here
        // needs its own file content naming itself, or every effect ends up
        // sharing whatever the reused file declares (see the sibling
        // "lists every effect on a layer" case just above, which hits the same
        // thing).
        constexpr const char* kBloomEffectFileJson = R"({
            "name": "bloom",
            "passes": [{ "material": "materials/_plain.json" }]
        })";
        constexpr const char* kGlowEffectFileJson  = R"({
            "name": "glow",
            "passes": [{ "material": "materials/_plain.json" }]
        })";
        auto                  vfs                  = makeAssetsVfsWith({
            { "/effects/bloom.json", kBloomEffectFileJson },
            { "/effects/glow.json", kGlowEffectFileJson },
        });
        const char*           kSceneJson           = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 506, "name": "user_bound_effect_layer",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "bloom",
          "visible": {"user": "godrays_enable", "value": true},
          "file": "effects/bloom.json" },
        { "id": 11, "name": "glow", "visible": true, "file": "effects/glow.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager   sm;
        WPUserProperties      props;
        // Override resolves "godrays_enable" FALSE at parse time -- this is
        // what makes the run cross the structural-skip red/green boundary.
        // An embedded-"value":true default with no override would already
        // build under today's code and prove nothing about this fix.
        REQUIRE(props.LoadFromProjectJson(
            R"({"general":{"properties":{"godrays_enable":{"value":false,"type":"bool"}}}})"));
        WPSceneParser parser;
        auto          scene = parser.Parse("scene_user_bound_effect", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(506) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(506);
        REQUIRE(effLayer->EffectCount() == 2u); // both built -- "bloom" is NOT structurally skipped

        std::shared_ptr<SceneImageEffect> bloom;
        for (std::size_t i = 0; i < effLayer->EffectCount(); i++) {
            if (effLayer->GetEffect(i)->name == "bloom") bloom = effLayer->GetEffect(i);
        }
        REQUIRE(bloom != nullptr);
        CHECK_FALSE(bloom->visible); // built but hidden, matching the resolved-false default
        for (auto& n : bloom->nodes) CHECK_FALSE(n.sceneNode->IsVisible());

        REQUIRE(scene->userPropEffectVisBindings.count("godrays_enable") == 1u);
        const auto& binding = scene->userPropEffectVisBindings.at("godrays_enable");
        REQUIRE(binding.size() == 1u);
        CHECK(binding[0].nodeId == 506);
        CHECK_FALSE(binding[0].defaultVisible);
        CHECK(effLayer->GetEffect((std::size_t)binding[0].effectIndex)->name == "bloom");
    }

    TEST_CASE(
        "a user-bound effect that would be the LAST one built keeps today's structural skip") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/bloom.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 507, "name": "terminal_user_bound_effect",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "bloom",
          "visible": {"user": "godrays_enable", "value": true},
          "file": "effects/bloom.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props;
        REQUIRE(props.LoadFromProjectJson(
            R"({"general":{"properties":{"godrays_enable":{"value":false,"type":"bool"}}}})"));
        WPSceneParser parser;
        auto scene = parser.Parse("scene_terminal_user_bound_effect", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        // Today's structural-skip behaviour, preserved: the only effect on
        // this layer is both user-bound-resolved-false AND would be last, so
        // the object ends up exactly like one whose sole effect is a plain
        // "visible": false -- no surviving effect means no effect layer is
        // built for it at all (countVisibleEffects gates assembleEffectChain
        // on at least one effect actually building).
        CHECK(scene->nodeEffectLayerMap.count(507) == 0);
        CHECK(scene->userPropEffectVisBindings.count("godrays_enable") == 0u);
    }

    TEST_CASE("an object whose ONLY effects are user-bound and all currently resolve false "
              "drops every one of them, not just the raw-JSON terminal") {
        // Regression guard for countVisibleEffects/adjustBlendForTextEffects's
        // hasEffect gate: it must count a user-bound effect that WOULD build
        // (Step 4's rule) even though its raw wpeffobj.visible resolved false,
        // or the whole assembleEffectChain call is skipped before assembleEffects
        // ever gets a chance to run at all.
        //
        // Both effects here are candidates for the terminal exclusion (both
        // bound, both currently false) -- excluding only the raw-JSON-last one
        // ("glow") would leave "bloom" force-built hidden, and "bloom" would
        // then become this layer's actual last BUILT effect once "glow" drops
        // out. That is exactly the state the terminal exclusion exists to
        // prevent (SceneImageEffectLayer::ResolveEffect picks the built list's
        // real last effect as its write target regardless of visibility), so
        // the exclusion has to cascade: once the raw-JSON-last candidate
        // drops, whichever candidate becomes the new last one is checked
        // against the same rule, and so on, until a genuinely visible one is
        // found or the object has none left.
        ensureGlslangInit();
        constexpr const char* kBloomEffectFileJson = R"({
            "name": "bloom",
            "passes": [{ "material": "materials/_plain.json" }]
        })";
        constexpr const char* kGlowEffectFileJson  = R"({
            "name": "glow",
            "passes": [{ "material": "materials/_plain.json" }]
        })";
        auto                  vfs                  = makeAssetsVfsWith({
            { "/effects/bloom.json", kBloomEffectFileJson },
            { "/effects/glow.json", kGlowEffectFileJson },
        });
        const char*           kSceneJson           = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 509, "name": "all_user_bound_effect_layer",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "bloom",
          "visible": {"user": "godrays_enable", "value": true},
          "file": "effects/bloom.json" },
        { "id": 11, "name": "glow",
          "visible": {"user": "godrays_enable", "value": true},
          "file": "effects/glow.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager   sm;
        WPUserProperties      props;
        REQUIRE(props.LoadFromProjectJson(
            R"({"general":{"properties":{"godrays_enable":{"value":false,"type":"bool"}}}})"));
        WPSceneParser parser;
        auto scene = parser.Parse("scene_all_user_bound_effect", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        // No effect survives -- exactly like an object whose sole effect is a
        // plain "visible": false: no surviving effect means no effect layer
        // is built for it at all (countVisibleEffects gates
        // assembleEffectChain on at least one effect actually building).
        CHECK(scene->nodeEffectLayerMap.count(509) == 0);
        CHECK(scene->userPropEffectVisBindings.count("godrays_enable") == 0u);
    }

    TEST_CASE("the terminal exclusion cascades past a non-terminal resolved-false candidate "
              "to stop at the nearest genuinely-visible effect") {
        // Three effects: a plain-visible base ("tint"), then two user-bound
        // effects both currently resolved false ("bloom" non-terminal,
        // "glow" the raw-JSON terminal). Excluding only "glow" (the old,
        // buggy single-index rule) would force-build "bloom" hidden, and
        // "bloom" would become the layer's actual last built effect once
        // "glow" drops -- the exact hidden-terminal-write-target hazard the
        // exclusion exists to prevent. The fix must exclude "bloom" too, and
        // stop the cascade at "tint" (genuinely visible, safe to be last).
        ensureGlslangInit();
        constexpr const char* kTintEffectFileJson  = R"({
            "name": "tint",
            "passes": [{ "material": "materials/_plain.json" }]
        })";
        constexpr const char* kBloomEffectFileJson = R"({
            "name": "bloom",
            "passes": [{ "material": "materials/_plain.json" }]
        })";
        constexpr const char* kGlowEffectFileJson  = R"({
            "name": "glow",
            "passes": [{ "material": "materials/_plain.json" }]
        })";
        auto                  vfs                  = makeAssetsVfsWith({
            { "/effects/tint.json", kTintEffectFileJson },
            { "/effects/bloom.json", kBloomEffectFileJson },
            { "/effects/glow.json", kGlowEffectFileJson },
        });
        const char*           kSceneJson           = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 510, "name": "cascade_user_bound_effect_layer",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "tint", "visible": true, "file": "effects/tint.json" },
        { "id": 11, "name": "bloom",
          "visible": {"user": "godrays_enable", "value": true},
          "file": "effects/bloom.json" },
        { "id": 12, "name": "glow",
          "visible": {"user": "godrays_enable", "value": true},
          "file": "effects/glow.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager   sm;
        WPUserProperties      props;
        REQUIRE(props.LoadFromProjectJson(
            R"({"general":{"properties":{"godrays_enable":{"value":false,"type":"bool"}}}})"));
        WPSceneParser parser;
        auto scene = parser.Parse("scene_cascade_user_bound_effect", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(510) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(510);
        // Only "tint" survives -- "bloom" and "glow" are both excluded, not
        // just "glow". If "bloom" were force-built here (the bug), it would
        // be this layer's actual last built effect and this REQUIRE would
        // see EffectCount() == 2u instead.
        REQUIRE(effLayer->EffectCount() == 1u);
        CHECK(effLayer->GetEffect(0)->name == "tint");
        CHECK(effLayer->GetEffect(0)->visible);
        CHECK(scene->userPropEffectVisBindings.count("godrays_enable") == 0u);
    }

    TEST_CASE("a user-bound effect whose material fails to load registers no runtime binding") {
        ensureGlslangInit();
        // No "/shaders/broken.vert" or "/shaders/broken.frag" mounted -- GetFileContent
        // returns "" for a missing path rather than throwing, so CompileToSpv
        // receives empty source and fails cleanly.
        constexpr const char* kBrokenMaterialEffectJson = R"({
            "name": "broken_material",
            "passes": [{ "material": "materials/broken.json" }]
        })";
        constexpr const char* kBrokenMaterialJson       = R"({
            "passes": [{"shader": "broken", "blending": "translucent", "textures": []}]
        })";
        auto                  vfs                       = makeAssetsVfsWith({
            { "/effects/broken.json", kBrokenMaterialEffectJson },
            { "/materials/broken.json", kBrokenMaterialJson },
            { "/effects/tint.json", kEffectFileJson },
        });
        const char*           kSceneJson                = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 508, "name": "broken_material_layer",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "broken_material",
          "visible": {"user": "godrays_enable", "value": true},
          "file": "effects/broken.json" },
        { "id": 11, "name": "tint", "visible": true, "file": "effects/tint.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager   sm;
        WPUserProperties      props {};
        WPSceneParser         parser;
        // No property override needed -- "value": true resolves true with no
        // scope active, so this fixture proves the eff_mat_ok gate specifically,
        // independent of the resolved-false path the case above covers.
        auto scene = parser.Parse("scene_broken_material", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(508) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(508);
        REQUIRE(effLayer->EffectCount() == 1u); // only "tint" -- "broken_material" dropped whole
        CHECK(effLayer->GetEffect(0)->name == "tint");
        CHECK(scene->userPropEffectVisBindings.count("godrays_enable") == 0u);
    }

    // The sibling case for the OTHER survivor on this line (`cxx_gt_to_ge`,
    // diverging from `context.ortho_w > 0` only at ortho_w exactly 0) is not
    // implemented: forcing `context.ortho_w == 0` requires an explicit
    // `orthogonalprojection` JSON object (Orthogonalprojection::FromJson only
    // ever runs when that key is present and non-null), and providing that key
    // -- for any value -- also sets `general.isOrtho = true`, switching the
    // scene off the perspective branch that builds this test's whole premise
    // (an activeCamera whose Width() stays at the untouched 1.0 default).
    // Verified by hand: in the resulting 2D scene, `scene.activeCamera` is
    // built by `ComputeOrthoCameraSize(ortho_w, ortho_h, zoom)`, which reuses
    // `ortho_w` itself as its own fallback when the quotient is non-positive --
    // so activeCamera->Width() reads back exactly 0, not 1.0, and the ternary's
    // two branches produce the identical value (0) whether or not the `>`
    // survives as `>=`.  The mutant is unreachable through any JSON this
    // parser's schema can produce; see the task report for the reproduction.

    TEST_CASE("a rotated shape-quad's final scale grows enough to cover the dominant "
              "viewport corner, by the hand-derived factor") {
        // The file's first-ever "shape" fixture -- a quad with no "image" key is the
        // only way to make wpimgobj.image.empty() true, which the rotation-coverage
        // gate requires.  Shape-quads exist to host procedural effects, so hasEffect
        // must be true (via a visible "effects" entry) for assembleEffectChain to run
        // at all.
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "camera": { "eye": "0 0 1000", "center": "0 0 0", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1000, "height": 800 } },
  "objects": [
    { "id": 506, "name": "rotated_quad_90deg", "shape": "quad",
      "origin": "500 400 0", "scale": "1 1 1", "angles": "0 0 1.5707963",
      "size": "900 900", "visible": true,
      "effects": [ { "id": 10, "name": "tint", "visible": true,
                     "file": "effects/tint.json" } ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_shape_quad_90deg", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(506) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(506);
        REQUIRE(effLayer != nullptr);

        // max_hx = max(500, 1000-500) = 500, max_hy = max(400, 800-400) = 400.
        // hw = hh = (900/2)*1 = 450.  At 90deg (ct=0, st=1):
        //   term1 = (500*0 + 400*1)/450 = 0.8889
        //   term2 = (500*1 + 400*0)/450 = 1.1111
        //   s = max(term1, term2) = 1.1111 = 10/9.
        const auto& scale = effLayer->FinalNode().Scale();
        CHECK(scale.x() == doctest::Approx(10.0 / 9.0).epsilon(1e-4));
        CHECK(scale.y() == doctest::Approx(10.0 / 9.0).epsilon(1e-4));
        CHECK(scale.z() == doctest::Approx(10.0 / 9.0).epsilon(1e-4));
    }

    TEST_CASE("a narrow scaled shape-quad kills the hw div_to_mul and the scale "
              "mul_to_div pair the 90-degree fixture cannot reach") {
        // The 90-degree fixture above picks the max_hy/hh term every time (its
        // symmetric canvas half-extents make hh's term dominate), so mutating hw's
        // own division, or either scale factor's multiplication (scale==1 makes
        // *1 and /1 numerically identical), never moves that fixture's `s`.  A
        // narrow-in-x quad at a shallow angle makes the hw-dependent term dominate
        // instead, and a non-1 scale (1.5) makes the multiply/divide swap visible.
        // Expected values were computed by running the real formula in float32
        // (not by hand) -- see the task report for the script and its output.
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "camera": { "eye": "0 0 1000", "center": "0 0 0", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1000, "height": 800 } },
  "objects": [
    { "id": 507, "name": "rotated_quad_narrow", "shape": "quad",
      "origin": "500 400 0", "scale": "1.5 1.5 1", "angles": "0 0 0.0174533",
      "size": "280 400", "visible": true,
      "effects": [ { "id": 10, "name": "tint", "visible": true,
                     "file": "effects/tint.json" } ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_shape_quad_narrow", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(507) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(507);
        REQUIRE(effLayer != nullptr);

        // max_hx=500, max_hy=400.  hw=(280/2)*1.5=210, hh=(400/2)*1.5=300.
        // ct=cos(0.0174533)=0.9998477, st=sin(0.0174533)=0.0174524.
        //   term1 = (500*ct + 400*st)/210 = 2.41383  <- dominant (hw-dependent)
        //   term2 = (500*st + 400*ct)/300 = 1.36222
        //   s = 2.41383.  FinalNode().Scale() = wpimgobj.scale * s (sc=(1.5,1.5,1)
        //   already carries the authored scale, so x/y = 1.5*s, z = 1.0*s).
        const auto& scale = effLayer->FinalNode().Scale();
        CHECK(scale.x() == doctest::Approx(3.620749).epsilon(1e-4));
        CHECK(scale.y() == doctest::Approx(3.620749).epsilon(1e-4));
        CHECK(scale.z() == doctest::Approx(2.413832).epsilon(1e-4));
    }

    TEST_CASE("a shape-quad at exactly the 0.01 radian rotation gate stays unscaled") {
        // The gate is `> 0.01f`, not `>=`.  At exactly 0.01 the block must not
        // run at all, so FinalNode().Scale() stays at the authored (1,1,1) --
        // this is the boundary the `cxx_gt_to_ge` mutant flips.
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "camera": { "eye": "0 0 1000", "center": "0 0 0", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1000, "height": 800 } },
  "objects": [
    { "id": 508, "name": "rotated_quad_boundary", "shape": "quad",
      "origin": "500 400 0", "scale": "1 1 1", "angles": "0 0 0.01",
      "size": "900 900", "visible": true,
      "effects": [ { "id": 10, "name": "tint", "visible": true,
                     "file": "effects/tint.json" } ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_shape_quad_boundary", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(508) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(508);
        REQUIRE(effLayer != nullptr);

        const auto& scale = effLayer->FinalNode().Scale();
        CHECK(scale.x() == doctest::Approx(1.0));
        CHECK(scale.y() == doctest::Approx(1.0));
        CHECK(scale.z() == doctest::Approx(1.0));
    }

    // -----------------------------------------------------------------------
    // A materially different concern from the sizing arithmetic above: the two
    // `parent_id >= 0` gates guarding SetInheritParent (WPSceneParser.cpp,
    // assembleEffectChain) are unbaselined survivors from the same commit.  No
    // fixture in the suite used a parent id of exactly zero -- the only value
    // where `>= 0` diverges from `> 0` -- or a parent id that names no real node
    // -- the only case where `node_map.count(...) > 0` diverges from `>= 0`
    // (always true for an unsigned count).
    // -----------------------------------------------------------------------

    TEST_CASE("kills the parent_id >= 0 gate on SetInheritParent when the parent id is "
              "exactly zero") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 0, "name": "root_group" },
    { "id": 700, "name": "effect_child_of_zero_parent", "parent": 0,
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [ { "id": 10, "name": "tint", "visible": true,
                     "file": "effects/tint.json" } ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_effect_child_of_zero_parent", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(700) == 1);
        CHECK(scene->nodeEffectLayerMap.at(700)->InheritParent() == true);
    }

    TEST_CASE("kills the node_map.count(...) > 0 gate on SetInheritParent when the "
              "parent id names no real node") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 701, "name": "effect_child_dangling_parent", "parent": 999,
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [ { "id": 10, "name": "tint", "visible": true,
                     "file": "effects/tint.json" } ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_effect_child_dangling_parent", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(701) == 1);
        CHECK(scene->nodeEffectLayerMap.at(701)->InheritParent() == false);
        CHECK(scene->attachmentProxyLinks.empty());
    }

    TEST_CASE("builds a parent-world proxy when an effect-child's parent was itself reset to "
              "identity by its own effect chain") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/tint.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 800, "name": "parent_with_effects", "image": "models/_plain.json",
      "origin": "300 150 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true,
      "effects": [ { "id": 10, "name": "tint", "visible": true,
                     "file": "effects/tint.json" } ] },
    { "id": 801, "name": "child_with_effects", "parent": 800,
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true,
      "effects": [ { "id": 11, "name": "tint2", "visible": true,
                     "file": "effects/tint.json" } ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_effect_child_of_effect_parent", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(801) == 1);
        auto it = std::find_if(
            scene->attachmentProxyLinks.begin(), scene->attachmentProxyLinks.end(), [](auto& l) {
                return l.child_id == 801;
            });
        REQUIRE(it != scene->attachmentProxyLinks.end());
        CHECK(it->parent_id == 800);
        CHECK(it->proxy->ModelTrans()(0, 3) == doctest::Approx(300.0));
        CHECK(it->proxy->ModelTrans()(1, 3) == doctest::Approx(150.0));
    }

    TEST_CASE("E2E: hide-pattern hides a matching particle object, "
              "leaves a non-matching image untouched") {
        ensureGlslangInit();
        LogCapture          log;
        auto                vfs        = makeAssetsVfsWith({
            { "/particle/dust.json",
              R"JSON({ "emitter": [{}], "renderer": [],
                        "material": "materials/_plain.json",
                        "children": [ { "name": "particle/dust_child.json" } ] })JSON" },
            { "/particle/dust_child.json",
              R"JSON({ "emitter": [{}], "renderer": [],
                        "material": "materials/_plain.json" })JSON" },
        });
        const char*         kSceneJson = R"JSON(
        {
          "general": { "clearcolor": "0 0 0",
                       "orthogonalprojection": { "width": 1280, "height": 720 } },
          "objects": [
            { "id": 300, "name": "keep_visible_layer", "image": "models/_plain.json",
              "origin": "0 0 0", "visible": true },
            { "id": 400, "name": "hideme_dust", "particle": "particle/dust.json",
              "origin": "0 0 0" }
          ]
        }
        )JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        parser.SetHidePattern("dust");
        auto scene = parser.Parse("scene_hide_particle", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        std::function<SceneNode*(SceneNode*, i32)> findById = [&](SceneNode* n,
                                                                  i32        id) -> SceneNode* {
            if (n->ID() == id) return n;
            for (auto& c : n->GetChildren())
                if (auto* hit = findById(c.get(), id)) return hit;
            return nullptr;
        };
        SceneNode* image = findById(scene->sceneGraph.get(), 300);
        SceneNode* dust  = findById(scene->sceneGraph.get(), 400);
        REQUIRE(image != nullptr);
        REQUIRE(dust != nullptr);
        CHECK(image->IsVisible());
        CHECK_FALSE(dust->IsVisible());

        int hide_log_count = 0;
        for (auto& l : LogCapture::lines())
            if (l.find("HidePattern: id=") != std::string::npos) hide_log_count++;
        CHECK(hide_log_count == 1);
    }

    TEST_CASE("E2E: hide-pattern hides a matching light object, "
              "leaves a non-matching light untouched") {
        LogCapture          log;
        auto                vfs        = makeEmptyAssetsVfs();
        const char*         kSceneJson = R"JSON(
        {
          "general": { "clearcolor": "0 0 0",
                       "orthogonalprojection": { "width": 1280, "height": 720 } },
          "objects": [
            { "id": 300, "name": "keep_visible_sun", "light": "point",
              "origin": "0 0 0", "color": "1 1 1", "radius": 1000.0, "intensity": 1.0 },
            { "id": 501, "name": "hideme_sun", "light": "point",
              "origin": "0 0 0", "color": "1 1 1", "radius": 1000.0, "intensity": 1.0 }
          ]
        }
        )JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        parser.SetHidePattern("hideme");
        auto scene = parser.Parse("scene_hide_light", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->lights.size() == 2);

        SceneLight* kept   = nullptr;
        SceneLight* hidden = nullptr;
        for (auto& l : scene->lights) {
            REQUIRE(l->node() != nullptr);
            if (l->node()->ID() == 300) kept = l.get();
            if (l->node()->ID() == 501) hidden = l.get();
        }
        REQUIRE(kept != nullptr);
        REQUIRE(hidden != nullptr);
        CHECK(kept->node()->IsVisible());
        CHECK_FALSE(hidden->node()->IsVisible());

        int hide_log_count = 0;
        for (auto& l : LogCapture::lines())
            if (l.find("HidePattern: id=") != std::string::npos) hide_log_count++;
        CHECK(hide_log_count == 1);
    }

    // ParseModelObj's own ApplyHidePattern call site — separate from the
    // particle case above, and the one place the hide has to reach a node
    // purely through inheritance: submesh children never get their own
    // SetVisible call, so IsVisible() has to walk the
    // m_visibility_parent chain up to the hidden parent to see it.
    TEST_CASE("E2E: hide-pattern hides a matching model object, cascading to its submesh child") {
        ensureGlslangInit();
        LogCapture          log;
        auto                vfs        = makeAssetsVfsWith({
            { "/models/_tri.mdl", makeTriangleModelMdl() },
        });
        const char*         kSceneJson = R"JSON(
        {
          "general": { "clearcolor": "0 0 0",
                       "orthogonalprojection": { "width": 1280, "height": 720 } },
          "objects": [
            { "id": 900, "name": "hideme_rock", "model": "models/_tri.mdl",
              "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true }
          ]
        }
        )JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        parser.SetHidePattern("rock");
        auto scene = parser.Parse("scene_hide_model", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        auto cit = scene->nodeById.find(900);
        REQUIRE(cit != scene->nodeById.end());
        SceneNode* parent = cit->second;
        REQUIRE(parent != nullptr);
        CHECK_FALSE(parent->IsVisible());
        REQUIRE_FALSE(parent->GetChildren().empty());
        CHECK_FALSE(parent->GetChildren().front()->IsVisible());

        int hide_log_count = 0;
        for (auto& l : LogCapture::lines())
            if (l.find("HidePattern: id=900") != std::string::npos) hide_log_count++;
        CHECK(hide_log_count == 1);
    }

    TEST_CASE("E2E: ParseModelObj honors JSON `visible:false`") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/models/_tri.mdl", makeTriangleModelMdl() },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 950, "name": "hidden_rock", "model": "models/_tri.mdl",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": false }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_invisible_model", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        auto cit = scene->nodeById.find(950);
        REQUIRE(cit != scene->nodeById.end());
        SceneNode* parent = cit->second;
        REQUIRE(parent != nullptr);
        CHECK_FALSE(parent->IsVisible());
        // Submesh children have no SetVisible of their own -- IsVisible() has
        // to walk the m_visibility_parent chain to the hidden parent, same as
        // the existing --hide-pattern case for this object type.
        REQUIRE_FALSE(parent->GetChildren().empty());
        CHECK_FALSE(parent->GetChildren().front()->IsVisible());
    }

    // ParseTextObj's placeholder-path ApplyHidePattern call: an empty font
    // takes the createPlaceholderNode() branch before rasterization is ever
    // attempted, so this is a genuinely different call site from the
    // rasterized-text case below, not just a different fixture on the same
    // line.
    TEST_CASE("E2E: hide-pattern hides a matching text object built via the placeholder path") {
        ensureGlslangInit();
        LogCapture          log;
        auto                vfs        = makeEmptyAssetsVfs();
        const char*         kSceneJson = R"JSON(
        {
          "general": { "clearcolor": "0 0 0",
                       "orthogonalprojection": { "width": 1280, "height": 720 } },
          "objects": [
            { "id": 901, "name": "hideme_caption",
              "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
              "text": { "value": "" }, "visible": true }
          ]
        }
        )JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        parser.SetHidePattern("caption");
        auto scene = parser.Parse("scene_hide_text_placeholder", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        auto cit = scene->nodeById.find(901);
        REQUIRE(cit != scene->nodeById.end());
        CHECK_FALSE(cit->second->IsVisible());

        int hide_log_count = 0;
        for (auto& l : LogCapture::lines())
            if (l.find("HidePattern: id=901") != std::string::npos) hide_log_count++;
        CHECK(hide_log_count == 1);
    }

    // ParseTextObj's rasterized main-path ApplyHidePattern call: a resolvable
    // font skips the placeholder branch and builds the mesh/material through
    // createTextSceneNode() instead — the fourth and last of the new call
    // sites, distinct from the placeholder path above.
    TEST_CASE("E2E: hide-pattern hides a matching text object built via the rasterized main "
              "path") {
        ensureGlslangInit();
        // FreeType needs a real face; the box supplies one via the
        // systemfont_* fallback resolver (same guard as the existing text
        // effect-chain E2E case above).
        if (ResolveSystemFontFallback("systemfont_sans").empty()) return;
        LogCapture          log;
        auto                vfs        = makeAssetsVfsWith({});
        const char*         kSceneJson = R"JSON(
        {
          "general": { "clearcolor": "0 0 0",
                       "orthogonalprojection": { "width": 1280, "height": 720 } },
          "objects": [
            { "id": 902, "name": "hideme_clock",
              "origin": "100 200 0", "scale": "1 1 1", "angles": "0 0 0",
              "size": "256 64",
              "font": "systemfont_sans", "pointsize": 24,
              "text": { "value": "12:00" },
              "visible": true }
          ]
        }
        )JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        parser.SetHidePattern("clock");
        auto scene = parser.Parse("scene_hide_text_main", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        auto cit = scene->nodeById.find(902);
        REQUIRE(cit != scene->nodeById.end());
        CHECK_FALSE(cit->second->IsVisible());

        int hide_log_count = 0;
        for (auto& l : LogCapture::lines())
            if (l.find("HidePattern: id=902") != std::string::npos) hide_log_count++;
        CHECK(hide_log_count == 1);
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
