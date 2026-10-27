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
#include "SceneParseFixtures.hpp"
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
#include "UserPropRuntimeUpdate.hpp"
#include "UserPropUseRecorder.hpp"
#include "WPUserProperties.hpp"
#include "wpscene/WPImageObject.h"
#include "wpscene/WPMaterial.h"
#include "wpscene/WPModelObject.h"
#include "wpscene/WPSoundObject.h"

#include "Audio/SoundManager.h"
#include "Utils/Logging.h"

#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"
#include "Fs/PhysicalFs.h"
#include "Interface/IImageParser.h"

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
using namespace wek::test;

namespace
{

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

// A flag-9 (pos + uv) model with one submesh per entry of mats, each with
// three vertices and one triangle, referencing that material path.
std::string makeModelMdlN(std::initializer_list<std::string_view> mats) {
    MdlBytes b;
    b.str("MDLV0013");
    b.i32(9); // mdl_flag: pos(3) + texcoord(2)
    b.i32(1);
    b.u32((uint32_t)mats.size()); // submesh_count
    for (auto m : mats) {
        b.str(m);
        b.i32(0);
        b.u32(3 * 5 * 4); // vertex bytes
        for (int i = 0; i < 3; i++) {
            b.f32((float)i);
            for (int k = 0; k < 4; k++) b.f32(0.0f);
        }
        b.u32(6); // one u16 triangle
        b.u16(0);
        b.u16(1);
        b.u16(2);
    }
    return b.data;
}

// One submesh referencing matPath (defaults to the shared trivial material).
std::string makeTriangleModelMdl(std::string_view matPath = "materials/_plain.json") {
    return makeModelMdlN({ matPath });
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

SceneNode* findNodeById(SceneNode* n, i32 id) {
    if (n->ID() == id) return n;
    for (auto& c : n->GetChildren()) {
        if (auto* hit = findNodeById(c.get(), id)) return hit;
    }
    return nullptr;
}

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
        const char*         kSceneJson = R"JSON(
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
        auto       scene = parser.Parse("scene_compose_3d", kSceneJson, *vfs, sm, props);
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

    TEST_CASE("E2E: text layer reading a composite RT by raw texture string keeps its source on "
              "screen and isolates its sprite") {
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

        // An ordinary reader does not hide its source: 401 keeps drawing itself
        // and 501 samples an isolated sprite copy of its chain output instead
        // of a full-framebuffer snapshot at 401's draw position.  Only a compose
        // layer's `dependencies` send a source offscreen.
        CHECK(dep->IsOffscreen() == false);
        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
        auto* depLayer = scene->nodeEffectLayerMap.at(401);
        CHECK(depLayer->IsIsolatedLink() == true);
        CHECK(depLayer->IsOffscreen() == false);
        REQUIRE(depLayer->EffectCount() == 1u);
        CHECK(depLayer->GetEffect(0)->synthesized == true);
        REQUIRE(scene->renderTargets.count(GenOffscreenRT(401)) == 1);
        CHECK(scene->renderTargets.at(GenOffscreenRT(401)).width == 256);
        CHECK(scene->renderTargets.at(GenOffscreenRT(401)).height == 256);
    }

    // ---- Isolated link sources --------------------------------------------
    // A layer read through `_rt_imageLayerComposite_<id>_a` by anything other
    // than a compose layer's `dependencies` keeps drawing itself on screen
    // (Gengar 3249016730's fire layer is both visible and blended into
    // another layer).  Readers get an isolated copy of its chain output, made
    // from a synthesized effectpassthrough appended as the on-screen terminal.
    namespace
    {
    constexpr const char* kPassReaderObjects = R"JSON(
      { "id": 401, "name": "dep_image",
        "image": "models/_plain.json",
        "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
        "color": "0.5 0.5 0.5", "alpha": 0.5,
        "visible": true },
      { "id": 501, "name": "reader",
        "image": "models/_plain.json",
        "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
        "visible": true,
        "effects": [
          { "id": 11, "name": "tint", "visible": true, "file": "effects/tint.json",
            "passes": [ { "textures": [ null, "_rt_imageLayerComposite_401_a" ] } ] }
        ] }
    )JSON";

    std::string isolatedLinkScene(const std::string& objects) {
        return std::string(R"JSON({
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [)JSON") +
               objects + "]}";
    }
    } // namespace

    TEST_CASE("E2E: an image layer read through an effect pass override keeps its source on "
              "screen and isolates it") {
        ensureGlslangInit();
        auto                vfs = makeIsolatedLinkVfs();
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse(
            "scene_pass_override_reader", isolatedLinkScene(kPassReaderObjects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        SceneNode* dep = findNodeById(scene->sceneGraph.get(), 401);
        REQUIRE(dep != nullptr);
        CHECK(dep->IsOffscreen() == false);

        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
        SceneImageEffectLayer* layer = scene->nodeEffectLayerMap.at(401);
        CHECK(layer->IsIsolatedLink() == true);
        CHECK(layer->IsOffscreen() == false);

        // A bare source gets the synthesized passthrough as its only effect.
        REQUIRE(layer->EffectCount() == 1u);
        auto synth = layer->GetEffect(0);
        CHECK(synth->synthesized == true);
        // The chain input slot holds the layer's first ping-pong buffer here;
        // ResolveEffect rebinds it per chain position when the graph is built.
        REQUIRE(synth->nodes.size() == 1u);
        REQUIRE(synth->nodes.front().sceneNode->Mesh() != nullptr);
        auto* mat = synth->nodes.front().sceneNode->Mesh()->Material();
        REQUIRE(mat != nullptr);
        REQUIRE(! mat->textures.empty());
        CHECK(mat->textures[0] == layer->FirstTarget());

        // The base pass already baked the layer's colour and alpha into the
        // buffer, so the passthrough must not apply them a second time.
        const auto& cv = mat->customShader.constValues;
        REQUIRE(cv.count("g_Color4") == 1u);
        for (int c = 0; c < 4; c++) CHECK(cv.at("g_Color4")[c] == doctest::Approx(1.0f));

        // _rt_offscreen_401 is only a size alias for the readers' resolution
        // lookups; nothing writes it.
        REQUIRE(scene->renderTargets.count(GenOffscreenRT(401)) == 1);
        CHECK(scene->renderTargets.at(GenOffscreenRT(401)).width == 256);
        CHECK(scene->renderTargets.at(GenOffscreenRT(401)).height == 256);
    }

    TEST_CASE("E2E: a link source with one real effect gets the synthesized passthrough appended "
              "last") {
        ensureGlslangInit();
        auto vfs = makeIsolatedLinkVfs();
        // 401 carries its own tint, and 501 reads it through a pass override.
        const std::string objects =
            R"JSON(
      { "id": 401, "name": "dep_image",
        "image": "models/_plain.json",
        "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
        "visible": true,
        "effects": [ { "id": 10, "name": "tint", "visible": true,
                       "file": "effects/tint.json" } ] },
    )JSON" + std::string(kPassReaderObjects)
                 .substr(std::string(kPassReaderObjects).find("{ \"id\": 501"));
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_source_with_effect", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
        auto* layer = scene->nodeEffectLayerMap.at(401);
        REQUIRE(layer->EffectCount() == 2u);
        CHECK(layer->GetEffect(0)->synthesized == false);
        CHECK(layer->GetEffect(1)->synthesized == true);
        // Scripts index effects by name/position; the synthesized one is not listed.
        REQUIRE(scene->layerEffectNames.count("dep_image") == 1);
        CHECK(scene->layerEffectNames.at("dep_image").size() == 1u);
    }

    TEST_CASE("E2E: a compose dependency also read by an ordinary reader stays offscreen-only") {
        ensureGlslangInit();
        auto vfs = makeIsolatedLinkVfs();
        // 402 lists 401 in `dependencies`; 501 also reads it through a pass
        // override.  The compose route wins: hidden, no isolated copy.
        const std::string   objects = std::string(kPassReaderObjects) + R"JSON(,
      { "id": 402, "name": "compose_layer",
        "image": "models/util/composelayer.json",
        "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
        "visible": true, "dependencies": [401] }
    )JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_compose_and_reader", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        SceneNode* dep = findNodeById(scene->sceneGraph.get(), 401);
        REQUIRE(dep != nullptr);
        CHECK(dep->IsOffscreen() == true);
        CHECK(scene->renderTargets.count(GenOffscreenRT(401)) == 1);
        // A bare offscreen dependency has no effect chain at all, so no layer
        // exists to carry an isolated-link flag or a synthesized passthrough.
        CHECK(scene->nodeEffectLayerMap.count(401) == 0);
    }

    TEST_CASE("E2E: an invisible source read by an ordinary reader keeps its offscreen route") {
        ensureGlslangInit();
        auto        vfs     = makeIsolatedLinkVfs();
        std::string objects = kPassReaderObjects;
        objects.replace(objects.find("\"visible\": true"), 15, "\"visible\": false");
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_invisible_source", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        SceneNode* dep = findNodeById(scene->sceneGraph.get(), 401);
        REQUIRE(dep != nullptr);
        CHECK(dep->IsOffscreen() == true);
        CHECK(scene->renderTargets.count(GenOffscreenRT(401)) == 1);
        CHECK(scene->nodeEffectLayerMap.count(401) == 0);
    }

    TEST_CASE("E2E: a compose-layer source read only by a texture string stays on screen") {
        ensureGlslangInit();
        auto vfs = makeIsolatedLinkVfs();
        // Only a compose layer's `dependencies` hide what they name.  A compose
        // layer that an ordinary effect reads keeps drawing itself, and is not
        // isolated: its content is the background it captures, so readers get
        // the framebuffer snapshot taken at its draw.  Forcing it offscreen
        // blacked out the six triangle panels of 2902406982, whose
        // clipping_mask readers sample them this way.
        std::string       objects    = kPassReaderObjects;
        const std::string plainImage = "\"image\": \"models/_plain.json\"";
        objects.replace(objects.find(plainImage),
                        plainImage.size(),
                        "\"image\": \"models/util/composelayer.json\"");
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_compose_source", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        SceneNode* src = findNodeById(scene->sceneGraph.get(), 401);
        REQUIRE(src != nullptr);
        CHECK(src->IsOffscreen() == false);
        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
        auto* layer = scene->nodeEffectLayerMap.at(401);
        CHECK(layer->IsOffscreen() == false);
        CHECK(layer->IsIsolatedLink() == false);
        CHECK(scene->renderTargets.count(GenOffscreenRT(401)) == 0);
    }

    TEST_CASE("E2E: a script-referenced source read by an ordinary reader is isolated") {
        ensureGlslangInit();
        auto                vfs     = makeIsolatedLinkVfs();
        const std::string   objects = R"JSON(
      { "id": 600, "name": "script_host",
        "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
        "visible": {
          "script": "function update(v){ return thisScene.getLayer('dep_image').visible; }",
          "scriptproperties": {},
          "value": true
        } },
    )JSON" + std::string(kPassReaderObjects);
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_scripted_source", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        SceneNode* dep = findNodeById(scene->sceneGraph.get(), 401);
        REQUIRE(dep != nullptr);
        CHECK(dep->IsOffscreen() == false);
        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
        CHECK(scene->nodeEffectLayerMap.at(401)->IsIsolatedLink() == true);
    }

    TEST_CASE("E2E: a no-effect fullscreen source stays a placeholder and gets no effect layer") {
        ensureGlslangInit();
        auto        vfs     = makeIsolatedLinkVfs();
        std::string objects = kPassReaderObjects;
        objects.replace(objects.find("models/_plain.json"), 18, "models/_full.json");
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_fullscreen_source", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        CHECK(scene->nodeEffectLayerMap.count(401) == 0);
    }

    // A zero-area layer cannot be isolated (no sprite to copy).  It keeps the
    // route it always had when something read it: offscreen, with a 1px target
    // so the link has something valid to point at.
    TEST_CASE("E2E: a zero-area source read by an ordinary reader is routed offscreen") {
        ensureGlslangInit();
        // No width/height in the model, so the object's own `size` applies.
        auto vfs = makeIsolatedLinkVfs(
            { { "/models/_nosize.json", R"({ "material": "materials/_plain.json" })" } });
        std::string objects = kPassReaderObjects;
        objects.replace(objects.find("models/_plain.json"), 18, "models/_nosize.json");
        const auto at = objects.find("\"name\": \"dep_image\",");
        REQUIRE(at != std::string::npos);
        objects.insert(at, "\"size\": \"0 0\", ");
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_zero_area_source", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        SceneNode* dep = findNodeById(scene->sceneGraph.get(), 401);
        REQUIRE(dep != nullptr);
        CHECK(dep->IsOffscreen() == true);
        REQUIRE(scene->renderTargets.count(GenOffscreenRT(401)) == 1);
        CHECK(scene->renderTargets.at(GenOffscreenRT(401)).width >= 1);
        CHECK(scene->renderTargets.at(GenOffscreenRT(401)).height >= 1);
        CHECK(scene->nodeEffectLayerMap.count(401) == 0);
    }

    // The area test is `>= 1`, so a source exactly one unit wide or tall still has a
    // sprite to copy and is isolated rather than sent offscreen.
    TEST_CASE("E2E: a source one unit wide or tall still counts as having area") {
        ensureGlslangInit();
        for (const char* sz : { "1 5", "5 1", "1 1" }) {
            CAPTURE(sz);
            auto vfs = makeIsolatedLinkVfs(
                { { "/models/_nosize.json", R"({ "material": "materials/_plain.json" })" } });
            std::string objects = kPassReaderObjects;
            objects.replace(objects.find("models/_plain.json"), 18, "models/_nosize.json");
            const auto at = objects.find("\"name\": \"dep_image\",");
            REQUIRE(at != std::string::npos);
            objects.insert(at, std::string("\"size\": \"") + sz + "\", ");
            audio::SoundManager sm;
            WPUserProperties    props {};
            WPSceneParser       parser;
            auto                scene =
                parser.Parse("scene_unit_area_source", isolatedLinkScene(objects), *vfs, sm, props);
            REQUIRE(scene != nullptr);

            SceneNode* dep = findNodeById(scene->sceneGraph.get(), 401);
            REQUIRE(dep != nullptr);
            CHECK(dep->IsOffscreen() == false);
            REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
            CHECK(scene->nodeEffectLayerMap.at(401)->IsIsolatedLink() == true);
        }
    }

    // Only a zero-area layer that something reads is forced offscreen; with no reader
    // it is left on its normal route.
    TEST_CASE("E2E: a zero-area layer nobody reads is not routed offscreen") {
        ensureGlslangInit();
        auto vfs = makeIsolatedLinkVfs(
            { { "/models/_nosize.json", R"({ "material": "materials/_plain.json" })" } });
        // Drop the reader: only the dep_image object remains.
        const std::string objects =
            R"JSON({ "id": 401, "name": "dep_image", "size": "0 0",
        "image": "models/_nosize.json",
        "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
        "color": "0.5 0.5 0.5", "alpha": 0.5,
        "visible": true })JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_zero_area_unread", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        SceneNode* dep = findNodeById(scene->sceneGraph.get(), 401);
        REQUIRE(dep != nullptr);
        CHECK(dep->IsOffscreen() == false);
    }

    // colorBlendMode adds an effectpassthrough that is already the chain's
    // on-screen terminal.  Appending the isolation passthrough after it would
    // demote it to a middle pass, which blends against the whole screen
    // squeezed into the sprite, so the chain is left as it is.
    TEST_CASE("E2E: a link source ending in the colorBlendMode passthrough gets no second "
              "passthrough") {
        ensureGlslangInit();
        auto        vfs     = makeIsolatedLinkVfs();
        std::string objects = kPassReaderObjects;
        const auto  at      = objects.find("\"visible\": true");
        REQUIRE(at != std::string::npos);
        objects.insert(at, "\"colorBlendMode\": 21, ");
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_blend_mode_source", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
        auto* layer = scene->nodeEffectLayerMap.at(401);
        CHECK(layer->IsIsolatedLink() == true);
        REQUIRE(layer->EffectCount() == 1u);
        CHECK(layer->GetEffect(0)->synthesized == false);
    }

    // An authored effect keeps the layer's colour and alpha in g_Color4; only a
    // pass that re-reads the base pass's output (the colorBlendMode
    // passthrough, or a synthesized one) gets them neutral, or they would be
    // applied twice.
    TEST_CASE("E2E: only a colorBlendMode passthrough gets a neutral g_Color4, not an authored "
              "effect") {
        ensureGlslangInit();
        auto                vfs     = makeIsolatedLinkVfs();
        const std::string   objects = R"JSON(
      { "id": 401, "name": "tinted",
        "image": "models/_plain.json",
        "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
        "color": "0.5 0.5 0.5", "alpha": 0.5, "colorBlendMode": 21,
        "visible": true,
        "effects": [
          { "id": 11, "name": "tint", "visible": true, "file": "effects/tint.json" }
        ] }
    )JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_color4_neutral", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
        auto* layer = scene->nodeEffectLayerMap.at(401);
        REQUIRE(layer->EffectCount() == 2u);
        auto color4Of = [&](usize i) {
            auto eff = layer->GetEffect(i);
            REQUIRE(eff->nodes.size() == 1u);
            auto* mat = eff->nodes.front().sceneNode->Mesh()->Material();
            REQUIRE(mat != nullptr);
            REQUIRE(mat->customShader.constValues.count("g_Color4") == 1u);
            return mat->customShader.constValues.at("g_Color4");
        };
        const auto tint = color4Of(0);
        CHECK(tint[0] == doctest::Approx(0.5f));
        CHECK(tint[3] == doctest::Approx(0.5f));
        const auto blend = color4Of(1);
        CHECK(blend[0] == doctest::Approx(1.0f));
        CHECK(blend[3] == doctest::Approx(1.0f));
    }

    // The isolation passthrough is stock material; if it cannot load, the
    // source falls back to the plain on-screen route instead of vanishing.
    TEST_CASE("E2E: a link source still parses when effectpassthrough.json is missing") {
        ensureGlslangInit();
        auto                vfs = makeAssetsVfsWith({
            { "/shaders/_s.vert", kTrivialVert },
            { "/shaders/_s.frag", kTwoSamplerFrag },
            { "/materials/_s.json",
              R"({ "passes": [{ "shader": "_s", "blending": "translucent", "textures": [] }] })" },
            { "/effects/tint.json",
              R"({ "name": "tint", "passes": [{ "material": "materials/_s.json" }] })" },
        });
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene = parser.Parse(
            "scene_no_passthrough_asset", isolatedLinkScene(kPassReaderObjects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        SceneNode* dep = findNodeById(scene->sceneGraph.get(), 401);
        REQUIRE(dep != nullptr);
        CHECK(dep->IsOffscreen() == false);
        CHECK(scene->nodeEffectLayerMap.count(401) == 0);
    }

    // A bare puppet source draws through a card base pass plus the puppet
    // composite.  The base pass carries the layer's colour, alpha and
    // brightness, so the composite must not multiply them in again.
    TEST_CASE("E2E: a bare puppet link source applies colour and alpha once") {
        ensureGlslangInit();
        auto        vfs     = makeIsolatedLinkVfs({
            { "/models/_ipup.json",
              R"({ "material": "materials/_i.json", "puppet": "models/_pup.mdl" })" },
            { "/models/_pup.mdl", makeOneBonePuppetMdl() },
        });
        std::string objects = kPassReaderObjects;
        objects.replace(objects.find("models/_plain.json"), 18, "models/_ipup.json");
        objects.insert(objects.find("\"name\": \"dep_image\","), "\"size\": \"256 256\", ");
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_bare_puppet_source", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
        auto* layer = scene->nodeEffectLayerMap.at(401);
        CHECK(layer->IsIsolatedLink() == true);
        REQUIRE(layer->EffectCount() == 1u);
        auto composite = layer->GetEffect(0);
        REQUIRE(composite->nodes.size() == 1u);
        auto* cmat = composite->nodes.front().sceneNode->Mesh()->Material();
        REQUIRE(cmat != nullptr);
        const auto& cv = cmat->customShader.constValues;
        REQUIRE(cv.count("g_Color4") == 1u);
        for (int c = 0; c < 4; c++) CHECK(cv.at("g_Color4")[c] == doctest::Approx(1.0f));
        REQUIRE(cv.count("g_UserAlpha") == 1u);
        CHECK(cv.at("g_UserAlpha")[0] == doctest::Approx(1.0f));
        REQUIRE(cv.count("g_Brightness") == 1u);
        CHECK(cv.at("g_Brightness")[0] == doctest::Approx(1.0f));

        // The base pass still carries them.
        SceneNode* dep = findNodeById(scene->sceneGraph.get(), 401);
        REQUIRE(dep != nullptr);
        auto* bmat = dep->Mesh()->Material();
        REQUIRE(bmat != nullptr);
        const auto& bv = bmat->customShader.constValues;
        REQUIRE(bv.count("g_Color4") == 1u);
        CHECK(bv.at("g_Color4")[0] == doctest::Approx(0.5f));
        CHECK(bv.at("g_Color4")[3] == doctest::Approx(0.5f));
    }

    // A puppet with effects draws its card through the effect chain, then the
    // skinned composite puts the result on screen.  The base pass already applied
    // the layer's colour, alpha and brightness, so the composite must not apply
    // them again: River Spirit 2513600306's "Girl reflection" (alpha 0.1)
    // vanished at 0.01.
    TEST_CASE("E2E: a puppet with effects applies colour, alpha and brightness once") {
        ensureGlslangInit();
        auto                vfs     = makeIsolatedLinkVfs({
            { "/models/_ipup.json",
              R"({ "material": "materials/_i.json", "puppet": "models/_pup.mdl" })" },
            { "/models/_pup.mdl", makeOneBonePuppetMdl() },
        });
        const std::string   objects = R"JSON(
      { "id": 401, "name": "puppet_fx",
        "image": "models/_ipup.json", "size": "256 256",
        "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
        "color": "0.5 0.5 0.5", "alpha": 0.5, "brightness": 1.2,
        "visible": true,
        "effects": [
          { "id": 11, "name": "tint", "visible": true, "file": "effects/tint.json" }
        ] }
    )JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_puppet_fx", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
        auto* layer = scene->nodeEffectLayerMap.at(401);
        CHECK(layer->IsIsolatedLink() == false);
        REQUIRE(layer->EffectCount() == 2u);
        CHECK(layer->GetEffect(0)->synthesized == false);
        auto composite = layer->GetEffect(1);
        CHECK(composite->synthesized == true);
        REQUIRE(composite->nodes.size() == 1u);
        auto* cmat = composite->nodes.front().sceneNode->Mesh()->Material();
        REQUIRE(cmat != nullptr);
        const auto& cv = cmat->customShader.constValues;
        REQUIRE(cv.count("g_Color4") == 1u);
        for (int c = 0; c < 4; c++) CHECK(cv.at("g_Color4")[c] == doctest::Approx(1.0f));
        REQUIRE(cv.count("g_UserAlpha") == 1u);
        CHECK(cv.at("g_UserAlpha")[0] == doctest::Approx(1.0f));
        REQUIRE(cv.count("g_Brightness") == 1u);
        CHECK(cv.at("g_Brightness")[0] == doctest::Approx(1.0f));

        SceneNode* node = findNodeById(scene->sceneGraph.get(), 401);
        REQUIRE(node != nullptr);
        auto* bmat = node->Mesh()->Material();
        REQUIRE(bmat != nullptr);
        const auto& bv = bmat->customShader.constValues;
        REQUIRE(bv.count("g_Color4") == 1u);
        CHECK(bv.at("g_Color4")[0] == doctest::Approx(0.5f));
        CHECK(bv.at("g_Color4")[3] == doctest::Approx(0.5f));
        REQUIRE(bv.count("g_Brightness") == 1u);
        CHECK(bv.at("g_Brightness")[0] == doctest::Approx(1.2f));
    }

    // The skinned composite takes the chain's output in slot 0.  A puppet whose
    // material lists no textures at all still needs that slot; indexing the
    // empty list aborted under libstdc++ assertions.
    TEST_CASE("E2E: a puppet with effects and a material without textures parses") {
        ensureGlslangInit();
        auto                vfs     = makeIsolatedLinkVfs({
            { "/materials/_i0.json",
              R"({ "passes": [{ "shader": "_i", "blending": "translucent", "textures": [] }] })" },
            { "/models/_ipup0.json",
              R"({ "material": "materials/_i0.json", "puppet": "models/_pup.mdl" })" },
            { "/models/_pup.mdl", makeOneBonePuppetMdl() },
        });
        const std::string   objects = R"JSON(
      { "id": 401, "name": "puppet_notex",
        "image": "models/_ipup0.json", "size": "256 256",
        "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
        "visible": true,
        "effects": [
          { "id": 11, "name": "tint", "visible": true, "file": "effects/tint.json" }
        ] }
    )JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_puppet_notex", isolatedLinkScene(objects), *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->nodeEffectLayerMap.count(401) == 1);
        auto* layer = scene->nodeEffectLayerMap.at(401);
        REQUIRE(layer->EffectCount() == 2u);
        auto composite = layer->GetEffect(1);
        CHECK(composite->synthesized == true);
        REQUIRE(composite->nodes.size() == 1u);
        auto* cmat = composite->nodes.front().sceneNode->Mesh()->Material();
        REQUIRE(cmat != nullptr);
        CHECK(cmat->textures.size() >= 1u);
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

    TEST_CASE("E2E: bloomhdrfeather is a fraction of the threshold, not an absolute knee") {
        ensureGlslangInit();
        auto vfs = makeAssetsVfsWith({
            { "/shaders/hdr_downsample.vert", kTrivialVert },
            { "/shaders/hdr_downsample.frag", kTrivialFrag },
            { "/shaders/combine_hdr.vert", kTrivialVert },
            { "/shaders/combine_hdr.frag", kTrivialFrag },
        });

        // Real-Time Earth (3557068717) authors exactly this pair. Reading
        // bloomhdrfeather as an absolute knee value put the knee past the
        // threshold (0.77 > 0.27), drove g_BloomBlendParams.y negative, and
        // widened the soft-knee transition zone until nearly the whole frame
        // fed the bloom-extract pass -- the HDR mip chain then compounded
        // that excess energy into blocky, quantised noise under ultra
        // postprocessing. The knee is the threshold scaled by the feather
        // fraction, matching the shipped hdr_downsample.frag's soft-knee
        // form.
        const char*         kSceneJson = R"JSON(
{
  "camera": { "eye": "0 0 1000", "center": "0 0 0", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1920, "height": 1080,
                                         "postprocessing": "ultra" },
               "hdr": true, "bloom": true, "bloomhdrthreshold": 0.27,
               "bloomhdrfeather": 0.77 },
  "objects": []
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_bloom_feather_fraction", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->bloomConfig.enabled);

        const auto& extract = scene->bloomConfig.nodes[0]->Mesh()->Material()->customShader;
        auto        knee    = extract.constValues.find("g_BloomBlendParams");
        REQUIRE(knee != extract.constValues.end());
        REQUIRE(knee->second.size() == 4);
        // knee = threshold * feather = 0.27 * 0.77 = 0.2079. The old
        // absolute-knee reading would have set .y to threshold - feather =
        // 0.27 - 0.77 = -0.50 instead.
        CHECK(knee->second[0] == doctest::Approx(0.27f));
        CHECK(knee->second[1] == doctest::Approx(0.0621f));
        CHECK(knee->second[2] == doctest::Approx(0.4158f));
        CHECK(knee->second[3] == doctest::Approx(1.202501f));
    }

    TEST_CASE("E2E: a bloomhdrfeather above 1.0 clamps to the full threshold") {
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
               "hdr": true, "bloom": true, "bloomhdrthreshold": 0.5,
               "bloomhdrfeather": 1.5 },
  "objects": []
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_bloom_feather_over_one", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->bloomConfig.enabled);

        const auto& extract = scene->bloomConfig.nodes[0]->Mesh()->Material()->customShader;
        auto        knee    = extract.constValues.find("g_BloomBlendParams");
        REQUIRE(knee != extract.constValues.end());
        REQUIRE(knee->second.size() == 4);
        // feather 1.5 clamps to 1.0, so knee == threshold: a hard cutoff at
        // the threshold, the same shape the zero-feather case has at the
        // opposite end of the fraction's range.
        CHECK(knee->second[0] == doctest::Approx(0.5f));
        CHECK(knee->second[1] == doctest::Approx(0.0f));
        CHECK(knee->second[2] == doctest::Approx(1.0f));
        CHECK(knee->second[3] == doctest::Approx(0.5f));
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

    TEST_CASE("E2E: a downsample pass's UV offset scales with its source mip, not just its "
              "resolution") {
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
               "hdr": true, "bloom": true },
  "objects": []
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_bloom_downsample_offset", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        REQUIRE(scene->bloomConfig.enabled);
        REQUIRE(scene->bloomConfig.nodes.size() == 8);

        // nodes[1] is the first cascade downsample (WE_BLOOM_MIP1 -> MIP2,
        // src_scale 0.5) -- its source is half res, so dx/dy must come out
        // double the full-res value, not the same as the Extract stage.
        // Picking a scale other than 1.0 is load-bearing: at scale 1.0 the
        // multiply and divide that compute dx/dy are indistinguishable
        // (x*1.0 == x/1.0), so this is the only stage that actually proves
        // which operator is in use.
        const auto& down = scene->bloomConfig.nodes[1]->Mesh()->Material()->customShader;
        auto        v    = down.constValues.find("g_RenderVar0");
        REQUIRE(v != down.constValues.end());
        REQUIRE(v->second.size() == 4);
        const float expectedDx = 0.5f / (1920.0f * 0.5f); // 0.5 / 960
        const float expectedDy = 0.5f / (1080.0f * 0.5f); // 0.5 / 540
        CHECK(v->second[0] == doctest::Approx(-expectedDx));
        CHECK(v->second[1] == doctest::Approx(-expectedDy));
        CHECK(v->second[2] == doctest::Approx(expectedDx));
        CHECK(v->second[3] == doctest::Approx(expectedDy));
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
        // what makes the run exercise the hidden-at-parse-time path.
        // An embedded "value":true with no override builds visible and would
        // not exercise this path.
        REQUIRE(props.LoadFromProjectJson(
            R"({"general":{"properties":{"godrays_enable":{"value":false,"type":"bool"}}}})"));
        WPSceneParser parser;
        auto          scene = parser.Parse("scene_user_bound_effect", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(506) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(506);
        REQUIRE(effLayer->EffectCount() ==
                2u); // both built: a bound effect that resolves hidden still builds

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

    TEST_CASE("records which effects were built hidden, and hands it to the layer init JSON") {
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
    { "id": 508, "name": "mixed_visibility_layer",
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
        REQUIRE(props.LoadFromProjectJson(
            R"({"general":{"properties":{"godrays_enable":{"value":false,"type":"bool"}}}})"));
        WPSceneParser parser;
        auto          scene = parser.Parse("scene_mixed_visibility", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->layerEffectVisible.count("mixed_visibility_layer") == 1);
        CHECK(scene->layerEffectVisible.at("mixed_visibility_layer") ==
              std::vector<bool> { false, true });

        auto init = nlohmann::json::parse(scene->SerializeLayerInitialStates());
        REQUIRE(init.contains("mixed_visibility_layer"));
        CHECK(init["mixed_visibility_layer"]["efv"].dump() == "[0,1]");
        CHECK(init["mixed_visibility_layer"]["efx"].dump() == "[\"bloom\",\"glow\"]");
    }

    TEST_CASE("a user-bound single effect that starts hidden is built, hidden, with its binding") {
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

        // A bound effect builds even when it resolves hidden and is the last
        // (here the only) effect, so a later toggle can show it without a
        // reload. While hidden, the last effect draws a passthrough, so the
        // layer still shows its own image.
        REQUIRE(scene->nodeEffectLayerMap.count(507) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(507);
        REQUIRE(effLayer->EffectCount() == 1u);
        CHECK_FALSE(effLayer->GetEffect(0)->visible);
        CHECK(scene->userPropEffectVisBindings.count("godrays_enable") == 1u);
    }

    TEST_CASE("an object whose only effects are user-bound and start hidden builds all of "
              "them hidden") {
        // countVisibleEffects/adjustBlendForTextEffects's hasEffect gate must
        // count a bound effect that WOULD build even though it resolved
        // hidden, or the whole assembleEffectChain call is skipped before
        // assembleEffects runs. Both effects are bound and hidden; both build
        // hidden so a toggle can show either without a reload, and the layer
        // shows its own image meanwhile (the hidden last effect draws a
        // passthrough).
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

        REQUIRE(scene->nodeEffectLayerMap.count(509) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(509);
        REQUIRE(effLayer->EffectCount() == 2u);
        CHECK_FALSE(effLayer->GetEffect(0)->visible);
        CHECK_FALSE(effLayer->GetEffect(1)->visible);
        CHECK(scene->userPropEffectVisBindings.count("godrays_enable") == 1u);
    }

    TEST_CASE("bound effects after a visible one are built hidden, not dropped") {
        // Three effects: a plain-visible base ("tint"), then two user-bound
        // effects both currently hidden ("bloom" in the middle, "glow" last).
        // Both bound effects build hidden after "tint" so a toggle can show
        // them without a reload; the hidden run at the end draws a
        // passthrough of "tint"'s output.
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
        REQUIRE(effLayer->EffectCount() == 3u);
        CHECK(effLayer->GetEffect(0)->name == "tint");
        CHECK(effLayer->GetEffect(0)->visible);
        CHECK_FALSE(effLayer->GetEffect(1)->visible);
        CHECK_FALSE(effLayer->GetEffect(2)->visible);
        CHECK(scene->userPropEffectVisBindings.count("godrays_enable") == 1u);
    }

    TEST_CASE("a bound effect whose binding cannot resolve to a property name is not built") {
        // A "user" field that is not a string and has no "name" gives no
        // property to toggle it by, so it can never be shown; building it
        // hidden at the tail would only blank the layer. It is treated like
        // a plain "visible": false, while a sibling with a valid binding
        // still builds.
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/bloom.json", kEffectFileJson },
            { "/effects/glow.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 511, "name": "unresolvable_binding_layer",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "bloom",
          "visible": {"user": "godrays_enable", "value": true},
          "file": "effects/bloom.json" },
        { "id": 11, "name": "glow",
          "visible": {"user": 42, "value": false},
          "file": "effects/glow.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props;
        REQUIRE(props.LoadFromProjectJson(
            R"({"general":{"properties":{"godrays_enable":{"value":false,"type":"bool"}}}})"));
        WPSceneParser parser;
        auto scene = parser.Parse("scene_unresolvable_binding", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(511) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(511);
        REQUIRE(effLayer->EffectCount() == 1u);
        CHECK_FALSE(effLayer->GetEffect(0)->visible); // the bound sibling, not the unresolvable one
        CHECK(scene->userPropEffectVisBindings.count("godrays_enable") == 1u);
    }

    TEST_CASE("a fullscreen layer whose only effects are bound and start hidden gets an effect "
              "layer, not the script-host placeholder") {
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/bloom.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 512, "name": "fullscreen_bound_layer",
      "image": "models/_plain.json",
      "fullscreen": true,
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
        auto          scene = parser.Parse("scene_fullscreen_bound", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(512) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(512);
        REQUIRE(effLayer->EffectCount() == 1u);
        CHECK_FALSE(effLayer->GetEffect(0)->visible);
        CHECK(scene->userPropEffectVisBindings.count("godrays_enable") == 1u);
        // Still reachable by id for getLayer().
        CHECK(scene->nodeById.count(512) == 1u);
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

    // Layers a SceneScript reaches with getEffect(...).visible build their
    // literal-hidden effects too (hidden), so the script can show them and
    // getEffect(name / index) finds them.
    TEST_CASE("a literal-hidden effect a script shows by name is built hidden and named") {
        ensureGlslangInit();
        auto        vfs        = makeAssetsVfsWith({
            { "/effects/noise.json",
                     R"({"name": "Noise", "passes": [{"material": "materials/_plain.json"}]})" },
        });
        const char* kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 801, "name": "pattern",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": { "value": true, "script":
        "export function update(v) { thisLayer.getEffect('Noise').visible = true; return v; }" },
      "effects": [
        { "id": 10, "name": "Noise", "visible": false, "file": "effects/noise.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props;
        WPSceneParser       parser;
        auto                scene = parser.Parse("scene_hidden_effect_shown", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(801) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(801);
        REQUIRE(effLayer->EffectCount() == 1u);
        CHECK_FALSE(effLayer->GetEffect(0)->visible);
        REQUIRE_FALSE(effLayer->GetEffect(0)->nodes.empty());
        for (const auto& n : effLayer->GetEffect(0)->nodes) {
            REQUIRE(n.sceneNode != nullptr);
            CHECK_FALSE(n.sceneNode->IsVisible());
        }
        REQUIRE(scene->layerEffectNames.count("pattern") == 1u);
        CHECK(scene->layerEffectNames.at("pattern") == std::vector<std::string> { "Noise" });
    }

    TEST_CASE("an all-hidden chain builds an effect layer when a script toggles it by index") {
        ensureGlslangInit();
        auto        vfs        = makeAssetsVfsWith({
            { "/effects/a.json",
                     R"({"name": "a", "passes": [{"material": "materials/_plain.json"}]})" },
            { "/effects/b.json",
                     R"({"name": "b", "passes": [{"material": "materials/_plain.json"}]})" },
            { "/effects/c.json",
                     R"({"name": "c", "passes": [{"material": "materials/_plain.json"}]})" },
        });
        const char* kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 802, "name": "all_hidden",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": { "value": true, "script":
        "export function update(v) { for (var i = 0; i < thisLayer.getEffectCount(); i++) { thisLayer.getEffect(i).visible = false; } return v; }" },
      "effects": [
        { "id": 10, "name": "a", "visible": false, "file": "effects/a.json" },
        { "id": 11, "name": "b", "visible": false, "file": "effects/b.json" },
        { "id": 12, "name": "c", "visible": false, "file": "effects/c.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props;
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_all_hidden_by_index", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(802) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(802);
        REQUIRE(effLayer->EffectCount() == 3u);
        for (usize i = 0; i < 3; i++) CHECK_FALSE(effLayer->GetEffect(i)->visible);
    }

    TEST_CASE("a script-targeted layer keeps authored effect order for index lookups") {
        // Script indices address the built list, so a hidden effect in the middle must not shift
        // the ones after it.
        ensureGlslangInit();
        auto        vfs        = makeAssetsVfsWith({
            { "/effects/a.json",
                     R"({"name": "a", "passes": [{"material": "materials/_plain.json"}]})" },
            { "/effects/b.json",
                     R"({"name": "b", "passes": [{"material": "materials/_plain.json"}]})" },
            { "/effects/c.json",
                     R"({"name": "c", "passes": [{"material": "materials/_plain.json"}]})" },
        });
        const char* kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 803, "name": "ordered",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": { "value": true, "script":
        "export function update(v) { thisLayer.getEffect(1).visible = true; return v; }" },
      "effects": [
        { "id": 10, "name": "a", "visible": true, "file": "effects/a.json" },
        { "id": 11, "name": "b", "visible": false, "file": "effects/b.json" },
        { "id": 12, "name": "c", "visible": true, "file": "effects/c.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props;
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_authored_order", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->layerEffectNames.count("ordered") == 1u);
        CHECK(scene->layerEffectNames.at("ordered") == std::vector<std::string> { "a", "b", "c" });
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(803);
        REQUIRE(effLayer->EffectCount() == 3u);
        CHECK(effLayer->GetEffect(0)->visible);
        CHECK_FALSE(effLayer->GetEffect(1)->visible);
        CHECK(effLayer->GetEffect(2)->visible);
    }

    TEST_CASE("a script-targeted layer lists only the effects that survive a failed shader") {
        // The script's effect index addresses the layer's live list, which
        // RemoveFailedEffects compacts after assembly; the name and visibility
        // lists have to follow, or getEffect(0) would toggle the wrong effect.
        ensureGlslangInit();
        constexpr const char* kBrokenFrag         = R"GLSL(
void main() {
    this is not glsl;
}
)GLSL";
        constexpr const char* kBrokenMaterialJson = R"({
            "passes": [{"shader": "_broken", "blending": "translucent", "textures": []}]
        })";
        auto                  vfs                 = makeAssetsVfsWith({
            { "/shaders/_broken.vert", kTrivialVert },
            { "/shaders/_broken.frag", kBrokenFrag },
            { "/materials/_broken.json", kBrokenMaterialJson },
            { "/effects/dead.json",
              R"({"name": "dead", "passes": [{"material": "materials/_broken.json"}]})" },
            { "/effects/b.json",
              R"({"name": "b", "passes": [{"material": "materials/_plain.json"}]})" },
            { "/effects/c.json",
              R"({"name": "c", "passes": [{"material": "materials/_plain.json"}]})" },
        });
        // A shader cache mount makes a failed compile yield an empty module set
        // on a live material, which RemoveFailedEffects reaps; without it the
        // material itself fails to load and the effect is never added.
        REQUIRE(vfs->Mount("/cache", std::make_unique<MemFs>(), "cache"));
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 804, "name": "reaped",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": { "value": true, "script":
        "export function update(v) { thisLayer.getEffect(0).visible = false; return v; }" },
      "effects": [
        { "id": 10, "name": "dead", "visible": true, "file": "effects/dead.json" },
        { "id": 11, "name": "b", "visible": false, "file": "effects/b.json" },
        { "id": 12, "name": "c", "visible": true, "file": "effects/c.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props;
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_reaped_effect_lists", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(804);
        REQUIRE(effLayer->EffectCount() == 2u);
        CHECK(effLayer->GetEffect(0)->name == "b");

        REQUIRE(scene->layerEffectNames.count("reaped") == 1u);
        CHECK(scene->layerEffectNames.at("reaped") == std::vector<std::string> { "b", "c" });
        REQUIRE(scene->layerEffectVisible.count("reaped") == 1u);
        CHECK(scene->layerEffectVisible.at("reaped") == std::vector<bool> { false, true });
    }

    TEST_CASE("a literal-hidden effect with no script stays unbuilt") {
        ensureGlslangInit();
        auto        vfs        = makeAssetsVfsWith({
            { "/effects/noise.json",
                     R"({"name": "Noise", "passes": [{"material": "materials/_plain.json"}]})" },
        });
        const char* kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 804, "name": "plain_hidden",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "Noise", "visible": false, "file": "effects/noise.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props;
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_hidden_unscripted", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);
        CHECK(scene->nodeEffectLayerMap.count(804) == 0u);
        CHECK(scene->scriptEffectVisLayers.count(804) == 0u);
    }

    TEST_CASE("a text layer's literal-hidden effect a script shows is built hidden") {
        ensureGlslangInit();
        if (ResolveSystemFontFallback("systemfont_sans").empty()) return;

        auto        vfs        = makeAssetsVfsWith({
            { "/effects/glow.json",
                     R"({"name": "glow", "passes": [{"material": "materials/_plain.json"}]})" },
        });
        const char* kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 805, "name": "script_text",
      "origin": "100 200 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "256 64",
      "font": "systemfont_sans", "pointsize": 24,
      "text": { "value": "12:00" },
      "visible": { "value": true, "script":
        "export function update(v) { thisLayer.getEffect('glow').visible = true; return v; }" },
      "effects": [
        { "id": 12, "name": "glow", "visible": false, "file": "effects/glow.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props;
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_text_hidden_shown", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(805) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(805);
        REQUIRE(effLayer->EffectCount() == 1u);
        CHECK_FALSE(effLayer->GetEffect(0)->visible);
        REQUIRE(scene->layerEffectNames.count("script_text") == 1u);
        CHECK(scene->layerEffectNames.at("script_text") == std::vector<std::string> { "glow" });
    }

    // A pass that animates a constant and carries a user binding, ahead of a
    // second pass chosen by the caller.  The binding sits in the material file
    // (a pass override records one too, covered separately), and the shader
    // annotates its uniform so the constant resolves to a GLSL name.
    TEST_CASE("an effect dropped for a failed later pass registers neither its uniform binding "
              "nor its constant animation") {
        ensureGlslangInit();
        constexpr const char* kPulseFrag             = R"GLSL(
uniform float g_Pulse; // {"material":"pulse","default":1}
void main() {
    gl_FragColor = vec4(g_Pulse);
}
)GLSL";
        constexpr const char* kAnimBoundMaterialJson = R"({
            "passes": [{
                "shader": "_anim", "blending": "translucent", "textures": [],
                "constantshadervalues": {
                    "pulse": {
                        "user": "myprop",
                        "animation": {
                            "c0": [{ "frame": 0, "value": 1 }, { "frame": 30, "value": 0 }],
                            "options": { "fps": 7.5, "length": 30, "mode": "single" }
                        },
                        "value": 1
                    }
                }
            }]
        })";
        constexpr const char* kBrokenMaterialJson    = R"({
            "passes": [{"shader": "broken", "blending": "translucent", "textures": []}]
        })";
        constexpr const char* kPairEffectJson        = R"({
            "name": "pair",
            "passes": [{ "material": "materials/_animbound.json" },
                       { "material": "materials/SECOND.json" }]
        })";
        constexpr const char* kSceneJson             = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 509, "name": "pair_layer",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "pair", "visible": true, "file": "effects/pair.json" },
        { "id": 11, "name": "tint", "visible": true, "file": "effects/tint.json" }
      ] }
  ]
}
)JSON";

        auto parseWithSecond = [&](const char* secondMaterial) {
            std::string effectJson = kPairEffectJson;
            effectJson.replace(effectJson.find("SECOND"), 6, secondMaterial);
            auto                vfs = makeAssetsVfsWith({
                { "/shaders/_anim.vert", kTrivialVert },
                { "/shaders/_anim.frag", kPulseFrag },
                { "/materials/_animbound.json", kAnimBoundMaterialJson },
                { "/materials/broken.json", kBrokenMaterialJson },
                { "/effects/pair.json", effectJson },
                { "/effects/tint.json", kEffectFileJson },
            });
            audio::SoundManager sm;
            WPUserProperties    props {};
            WPSceneParser       parser;
            return parser.Parse("scene_pair_effect", kSceneJson, *vfs, sm, props);
        };

        SUBCASE("the second pass fails to load") {
            auto scene = parseWithSecond("broken");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->nodeEffectLayerMap.count(509) == 1);
            REQUIRE(scene->nodeEffectLayerMap.at(509)->EffectCount() == 1u); // only "tint"

            CHECK(scene->userPropUniformBindings.count("myprop") == 0u);
            CHECK(scene->effectUniformAnimations.empty());
        }

        SUBCASE("both passes load") {
            auto scene = parseWithSecond("_plain");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->nodeEffectLayerMap.count(509) == 1);
            REQUIRE(scene->nodeEffectLayerMap.at(509)->EffectCount() == 2u);

            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            CHECK(scene->userPropUniformBindings.at("myprop").size() == 1u);
            CHECK(scene->effectUniformAnimations.size() == 1u);
        }
    }

    // A {"user": ...} constant written in the scene's own per-effect passes[]
    // override (the usual authoring shape) has to reach the runtime bindings
    // the same way one authored in the material file does, or changing the
    // property reparses the whole scene instead of updating the uniform.
    TEST_CASE("a user-bound constant in a scene effect's pass override registers a uniform "
              "binding") {
        ensureGlslangInit();
        constexpr const char* kTwoUniformFrag      = R"GLSL(
uniform float g_Pulse; // {"material":"pulse","default":1}
uniform float g_Other; // {"material":"other","default":1}
void main() {
    gl_FragColor = vec4(g_Pulse + g_Other);
}
)GLSL";
        constexpr const char* kUnboundMaterialJson = R"({
            "passes": [{ "shader": "_anim", "blending": "translucent", "textures": [] }]
        })";
        constexpr const char* kBoundMaterialJson   = R"({
            "passes": [{
                "shader": "_anim", "blending": "translucent", "textures": [],
                "constantshadervalues": { "pulse": { "user": "fileprop", "value": 1 } }
            }]
        })";
        constexpr const char* kEffectJson          = R"({
            "name": "pulse",
            "passes": [{ "material": "materials/MATERIAL.json" }]
        })";

        // `passOverride` is the body of the scene effect's passes[0] entry.
        // `effectFile` replaces the effect file when non-empty; `objectExtra` is spliced into
        // the object (used for an image's "instance" block).
        auto parseWith = [&](const char*        material,
                             const std::string& passOverride,
                             bool               text        = false,
                             const std::string& effectFile  = {},
                             const std::string& objectExtra = {},
                             const char*        imageFile   = "models/_plain.json") {
            std::string effectJson = effectFile.empty() ? std::string(kEffectJson) : effectFile;
            if (effectFile.empty()) effectJson.replace(effectJson.find("MATERIAL"), 8, material);
            auto        vfs     = makeAssetsVfsWith({
                { "/models/_inst.json",
                  R"({ "material": "materials/_unbound.json", "width": 256, "height": 256 })" },
                { "/shaders/_anim.vert", kTrivialVert },
                { "/shaders/_anim.frag", kTwoUniformFrag },
                { "/materials/_unbound.json", kUnboundMaterialJson },
                { "/materials/_bound.json", kBoundMaterialJson },
                { "/effects/pulse.json", effectJson },
            });
            std::string scene   = R"JSON({
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [ OBJECT ]
})JSON";
            std::string effects = R"JSON("effects": [
        { "id": 10, "name": "pulse", "visible": true, "file": "effects/pulse.json",
          "passes": [ PASS ] } ])JSON";
            effects.replace(effects.find("PASS"), 4, passOverride);
            std::string object = text ? R"JSON({ "id": 520, "name": "obj",
      "origin": "100 200 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "256 64", "font": "systemfont_sans", "pointsize": 24,
      "text": { "value": "12:00" }, "visible": true, )JSON" +
                                            effects + " }"
                                      : R"JSON({ "id": 520, "name": "obj", "image": "IMAGE",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true, )JSON" +
                                            objectExtra + effects + " }";
            if (auto at = object.find("IMAGE"); at != std::string::npos)
                object.replace(at, 5, imageFile);
            scene.replace(scene.find("OBJECT"), 6, object);
            audio::SoundManager sm;
            WPUserProperties    props {};
            WPSceneParser       parser;
            return parser.Parse("scene_pass_override_binding", scene, *vfs, sm, props);
        };

        SUBCASE("the binding sits in the scene pass override") {
            auto scene = parseWith(
                "_unbound",
                R"({ "constantshadervalues": { "pulse": { "user": "myprop", "value": 1 } } })");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            REQUIRE(scene->userPropUniformBindings.at("myprop").size() == 1u);
            CHECK(scene->userPropUniformBindings.at("myprop")[0].uniformName == "g_Pulse");
        }

        SUBCASE("one property driving two constants binds both") {
            auto scene = parseWith("_unbound", R"({ "constantshadervalues": {
                "pulse": { "user": "myprop", "value": 1 },
                "other": { "user": "myprop", "value": 1 } } })");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            const auto& bs = scene->userPropUniformBindings.at("myprop");
            REQUIRE(bs.size() == 2u);
            CHECK(((bs[0].uniformName == "g_Pulse" && bs[1].uniformName == "g_Other") ||
                   (bs[0].uniformName == "g_Other" && bs[1].uniformName == "g_Pulse")));
        }

        SUBCASE("a plain value in the override replaces a constant the material file bound") {
            auto scene = parseWith("_bound", R"({ "constantshadervalues": { "pulse": 0.5 } })");
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropUniformBindings.count("fileprop") == 0u);
        }

        SUBCASE("a differently bound value in the override replaces the material file's binding") {
            auto scene = parseWith(
                "_bound",
                R"({ "constantshadervalues": { "pulse": { "user": "myprop", "value": 1 } } })");
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropUniformBindings.count("fileprop") == 0u);
            CHECK(scene->userPropUniformBindings.count("myprop") == 1u);
        }

        SUBCASE("a user constant on the effect file's own pass binds") {
            auto scene = parseWith("_unbound", "{}", false, R"({
                "name": "pulse",
                "passes": [{ "material": "materials/_unbound.json",
                    "constantshadervalues": { "pulse": { "user": "myprop", "value": 1 } } }]
            })");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            REQUIRE(scene->userPropUniformBindings.at("myprop").size() == 1u);
            CHECK(scene->userPropUniformBindings.at("myprop")[0].uniformName == "g_Pulse");
        }

        SUBCASE("the same property on both passes of an effect binds each pass's mesh") {
            const std::string bound =
                R"({ "constantshadervalues": { "pulse": { "user": "myprop", "value": 1 } } })";
            auto scene = parseWith("_unbound", bound + ", " + bound, false, R"({
                "name": "pulse",
                "passes": [{ "material": "materials/_unbound.json" },
                           { "material": "materials/_unbound.json" }]
            })");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            const auto& bs = scene->userPropUniformBindings.at("myprop");
            REQUIRE(bs.size() == 2u);
            CHECK(bs[0].mesh != bs[1].mesh);
        }

        SUBCASE("an image's instance block binds through the base-image path") {
            // The pass override is irrelevant here; the object carries the binding.
            auto scene = parseWith("_unbound",
                                   "{}",
                                   false,
                                   {},
                                   R"JSON(
                "instance": { "constantshadervalues": { "pulse": { "user": "myprop", "value": 1 } } },
)JSON",
                                   "models/_inst.json");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
        }

        SUBCASE("a text layer's effect override binds the same way") {
            if (ResolveSystemFontFallback("systemfont_sans").empty()) {
                MESSAGE("skipped: no system font fallback for systemfont_sans");
                return;
            }
            auto scene = parseWith(
                "_unbound",
                R"({ "constantshadervalues": { "pulse": { "user": "myprop", "value": 1 } } })",
                true);
            REQUIRE(scene != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            CHECK(scene->userPropUniformBindings.at("myprop")[0].uniformName == "g_Pulse");
        }
    }

    // ------------------------------------------------------------------
    // Properties that feed a field parsing consumes once.  A change to one
    // of those needs a reload even when the same property also has a live
    // binding, so the parser records them in Scene::userPropStaticUses and
    // wek::userPropsNeedingReload consults that.
    // ------------------------------------------------------------------

    static std::shared_ptr<Scene> parseStaticUseScene(
        const std::string&                                         objects,
        const char*                                                propertiesJson,
        std::initializer_list<std::pair<std::string, std::string>> extras = {}) {
        ensureGlslangInit();
        auto        vfs   = makeAssetsVfsWith(extras);
        std::string scene = R"JSON({
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [ OBJECTS ]
})JSON";
        scene.replace(scene.find("OBJECTS"), 7, objects);
        WPUserProperties props {};
        REQUIRE(props.LoadFromProjectJson(std::string(R"({"general":{"properties":)") +
                                          propertiesJson + "}}"));
        audio::SoundManager sm;
        WPSceneParser       parser;
        return parser.Parse("scene_static_use", scene, *vfs, sm, props);
    }

    static std::string imageObject(const std::string& visible, const std::string& extra = {}) {
        return R"JSON({ "id": 601, "name": "layer", "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": )JSON" +
               visible + extra + " }";
    }

    constexpr const char* kShowMeOff = R"({"showme":{"value":false,"type":"bool"}})";
    constexpr const char* kShowMeOn  = R"({"showme":{"value":true,"type":"bool"}})";
    constexpr const char* kVisShowMe = R"({"user":"showme","value":true})";

    constexpr const char* kStaticTwoUniformFrag      = R"GLSL(
uniform float g_Pulse; // {"material":"pulse","default":1}
uniform float g_Other; // {"material":"other","default":1}
void main() {
    gl_FragColor = vec4(g_Pulse + g_Other);
}
)GLSL";
    constexpr const char* kStaticUnboundMaterialJson = R"({
        "passes": [{ "shader": "_anim", "blending": "translucent", "textures": [] }]
    })";
    constexpr const char* kStaticPulseEffectJson     = R"({
        "name": "pulse",
        "passes": [{ "material": "materials/_unbound.json" }]
    })";

    TEST_CASE("a property bound as a pass-override constant and read as an object color reloads") {
        const std::string effects = R"JSON(, "effects": [
        { "id": 10, "name": "pulse", "visible": true, "file": "effects/pulse.json",
          "passes": [ { "constantshadervalues": { "pulse": { "user": "myprop", "value": 1 } } } ] }
        ])JSON";
        const std::string color   = R"JSON(, "color": { "user": "myprop", "value": "1 1 1" })JSON";

        auto parse = [&](const std::string& extra) {
            return parseStaticUseScene(
                imageObject("true", extra),
                R"({"myprop":{"value":1,"type":"slider"}})",
                {
                    { "/shaders/_anim.vert", kTrivialVert },
                    { "/shaders/_anim.frag", kStaticTwoUniformFrag },
                    { "/materials/_unbound.json", kStaticUnboundMaterialJson },
                    { "/effects/pulse.json", kStaticPulseEffectJson },
                });
        };

        SUBCASE("the constant alone is live") {
            auto scene = parse(effects);
            REQUIRE(scene != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            CHECK(scene->userPropStaticUses.count("myprop") == 0u);
            CHECK(wek::userPropsNeedingReload(*scene, { "myprop" }).empty());
        }

        SUBCASE("the object color alone is a static use") {
            auto scene = parse(color);
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropUniformBindings.count("myprop") == 0u);
            CHECK(scene->userPropStaticUses.count("myprop") == 1u);
            CHECK_FALSE(wek::userPropsNeedingReload(*scene, { "myprop" }).empty());
        }

        SUBCASE("both together bind the uniform and still reload") {
            auto scene = parse(color + effects);
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropUniformBindings.count("myprop") == 1u);
            CHECK(scene->userPropStaticUses.count("myprop") == 1u);
            CHECK_FALSE(wek::userPropsNeedingReload(*scene, { "myprop" }).empty());
        }
    }

    TEST_CASE("a conditional pass-override constant is neither bound nor lost") {
        // The live update cannot evaluate the condition, so the pass override
        // leaves it unbound; it has to surface as a static use or a change to
        // the property would neither apply nor reload.
        const std::string effects = R"JSON(, "effects": [
        { "id": 10, "name": "pulse", "visible": true, "file": "effects/pulse.json",
          "passes": [ { "constantshadervalues": { "pulse": {
              "user": { "name": "myprop", "condition": "1" }, "value": 1 } } } ] }
        ])JSON";
        auto              scene =
            parseStaticUseScene(imageObject("true", effects),
                                R"({"myprop":{"value":1,"type":"combo"}})",
                                {
                                    { "/shaders/_anim.vert", kTrivialVert },
                                    { "/shaders/_anim.frag", kStaticTwoUniformFrag },
                                    { "/materials/_unbound.json", kStaticUnboundMaterialJson },
                                    { "/effects/pulse.json", kStaticPulseEffectJson },
                                });
        REQUIRE(scene != nullptr);
        CHECK(scene->userPropUniformBindings.count("myprop") == 0u);
        CHECK(scene->userPropStaticUses.count("myprop") == 1u);
    }

    TEST_CASE("an image visible binding that resolves true is live, not a static use") {
        auto scene = parseStaticUseScene(imageObject(kVisShowMe), kShowMeOn);
        REQUIRE(scene != nullptr);
        CHECK(scene->userPropVisBindings.count("showme") == 1u);
        CHECK(scene->userPropStaticUses.count("showme") == 0u);
        CHECK(wek::userPropsNeedingReload(*scene, { "showme" }).empty());
    }

    // A layer that starts hidden is routed to a private render target at parse
    // time and skipped from the main composite.  The runtime visibility path
    // only flips the node's flag; it never re-routes, so switching the property
    // on would show nothing.  That read has to count as baked in.
    TEST_CASE("an image routed offscreen by its visible binding reloads when the property "
              "changes") {
        auto scene = parseStaticUseScene(imageObject(kVisShowMe), kShowMeOff);
        REQUIRE(scene != nullptr);
        CHECK(scene->userPropVisBindings.count("showme") == 1u);
        CHECK(scene->userPropStaticUses.count("showme") == 1u);
        CHECK_FALSE(wek::userPropsNeedingReload(*scene, { "showme" }).empty());
    }

    TEST_CASE("a combo-selector image that starts hidden stays in the main graph and live") {
        auto scene = parseStaticUseScene(
            imageObject(R"({"user":{"name":"showme","condition":"2"},"value":true})"),
            R"({"showme":{"value":"1","type":"combo"}})");
        REQUIRE(scene != nullptr);
        CHECK(scene->userPropVisBindings.count("showme") == 1u);
        CHECK(scene->userPropStaticUses.count("showme") == 0u);
    }

    TEST_CASE("object-level visible bindings on a group and a light are live") {
        const std::string objects = R"JSON(
    { "id": 700, "name": "grp", "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": { "user": "showme", "value": true } },
    { "id": 701, "name": "lamp", "light": "point", "color": "1 1 1",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "radius": 100.0,
      "intensity": 1.0, "visible": { "user": "lampprop", "value": true } })JSON";
        auto              scene   = parseStaticUseScene(
            objects,
            R"({"showme":{"value":true,"type":"bool"},"lampprop":{"value":true,"type":"bool"}})");
        REQUIRE(scene != nullptr);
        CHECK(scene->userPropVisBindings.count("showme") == 1u);
        CHECK(scene->userPropVisBindings.count("lampprop") == 1u);
        CHECK(scene->userPropStaticUses.empty());
    }

    TEST_CASE("object-level visible bindings on a model and a particle are live") {
        const std::string objects = R"JSON(
    { "id": 702, "name": "rock", "model": "models/_tri.mdl",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": { "user": "rockprop", "value": true } },
    { "id": 703, "name": "dust", "particle": "particle/dust.json", "origin": "0 0 0",
      "visible": { "user": "dustprop", "value": true } })JSON";
        auto              scene   = parseStaticUseScene(
            objects,
            R"({"rockprop":{"value":true,"type":"bool"},"dustprop":{"value":true,"type":"bool"}})",
            {
                { "/models/_tri.mdl", makeTriangleModelMdl() },
                { "/particle/dust.json",
                  R"JSON({ "emitter": [{ "name": "box", "id": 1 }],
                           "renderer": [{ "name": "sprite" }],
                           "material": "materials/_plain.json",
                           "maxcount": 4 })JSON" },
            });
        REQUIRE(scene != nullptr);
        CHECK(scene->userPropVisBindings.count("rockprop") == 1u);
        CHECK(scene->userPropVisBindings.count("dustprop") == 1u);
        CHECK(scene->userPropStaticUses.count("rockprop") == 0u);
        CHECK(scene->userPropStaticUses.count("dustprop") == 0u);
    }

    TEST_CASE("an effect visible binding is live, not a static use") {
        auto scene = parseStaticUseScene(
            imageObject("true",
                        R"JSON(, "effects": [ { "id": 10, "name": "tint", "visible":
                            { "user": "fxprop", "value": true },
                            "file": "effects/tint.json" } ])JSON"),
            R"({"fxprop":{"value":true,"type":"bool"}})",
            { { "/effects/tint.json", kEffectFileJson } });
        REQUIRE(scene != nullptr);
        CHECK(scene->userPropEffectVisBindings.count("fxprop") == 1u);
        CHECK(scene->userPropStaticUses.count("fxprop") == 0u);
    }

    TEST_CASE("a text pointsize bound by name is live; a conditional one is baked in") {
        if (ResolveSystemFontFallback("systemfont_sans").empty()) {
            MESSAGE("skipped: no system font fallback for systemfont_sans");
            return;
        }
        auto text = [](const std::string& pointsize) {
            return R"JSON({ "id": 720, "name": "t", "origin": "100 200 0", "scale": "1 1 1",
      "angles": "0 0 0", "size": "256 64", "font": "systemfont_sans", "pointsize": )JSON" +
                   pointsize + R"JSON(, "text": { "value": "12:00" }, "visible": true })JSON";
        };
        SUBCASE("by name") {
            auto scene = parseStaticUseScene(text(R"({"user":"sizeprop","value":24})"),
                                             R"({"sizeprop":{"value":24,"type":"slider"}})");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->textLayers.size() == 1u);
            CHECK(scene->textLayers[0].pointsizeUserProp == "sizeprop");
            CHECK(scene->userPropStaticUses.count("sizeprop") == 0u);
        }
        SUBCASE("conditional") {
            auto scene = parseStaticUseScene(
                text(R"({"user":{"name":"sizeprop","condition":"1"},"value":24})"),
                R"({"sizeprop":{"value":1,"type":"combo"}})");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->textLayers.size() == 1u);
            CHECK(scene->textLayers[0].pointsizeUserProp.empty());
            CHECK(scene->userPropStaticUses.count("sizeprop") == 1u);
        }
    }

    TEST_CASE("fields parsing consumes once are static uses") {
        SUBCASE("an object color") {
            auto scene = parseStaticUseScene(
                imageObject("true", R"(, "color": { "user": "c", "value": "1 1 1" })"),
                R"({"c":{"value":"1 1 1","type":"color"}})");
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropStaticUses.count("c") == 1u);
        }
        SUBCASE("an object parallax depth") {
            auto scene = parseStaticUseScene(
                imageObject("true", R"(, "parallaxDepth": { "user": "d", "value": "0 0" })"),
                R"({"d":{"value":"0 0","type":"textinput"}})");
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropStaticUses.count("d") == 1u);
        }
        SUBCASE("an animation layer's visible") {
            wpscene::WPImageObject obj;
            UserPropUseRecorder    rec;
            auto                   vfs = makeAssetsVfsWith({});
            {
                UserPropUseRecordingScope scope(&rec);
                obj.FromJson(nlohmann::json::parse(R"({
                    "id": 1, "name": "puppet", "image": "models/_plain.json",
                    "animationlayers": [ { "animation": 1, "blend": 1.0, "rate": 1.0,
                                           "visible": { "user": "layervis", "value": true } } ]
                })"),
                             *vfs);
            }
            CHECK(rec.uses.count("layervis") == 1u);
        }
        SUBCASE("a scene general setting") {
            ensureGlslangInit();
            auto             vfs       = makeAssetsVfsWith({});
            const char*      sceneJson = R"JSON({
  "camera": { "center": "0 0 0", "eye": "0 0 1", "up": "0 1 0" },
  "general": { "clearcolor": "0 0 0", "bloom": { "user": "bloomprop", "value": true },
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": []
})JSON";
            WPUserProperties props {};
            REQUIRE(props.LoadFromProjectJson(
                R"({"general":{"properties":{"bloomprop":{"value":true,"type":"bool"}}}})"));
            audio::SoundManager sm;
            WPSceneParser       parser;
            auto scene = parser.Parse("scene_general_static", sceneJson, *vfs, sm, props);
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropStaticUses.count("bloomprop") == 1u);
        }
    }

    TEST_CASE("material fields parsing consumes once are static uses") {
        auto record = [](const char* materialJson) {
            wpscene::WPMaterial mat;
            UserPropUseRecorder rec;
            {
                UserPropUseRecordingScope scope(&rec);
                REQUIRE(mat.FromJson(nlohmann::json::parse(materialJson)));
            }
            return rec;
        };
        SUBCASE("a combo value") {
            auto rec = record(R"({"passes":[{"shader":"_t","combos":
                {"MODE":{"user":"modeprop","value":1}}}]})");
            CHECK(rec.uses.count("modeprop") == 1u);
        }
        SUBCASE("a texture path") {
            auto rec = record(R"({"passes":[{"shader":"_t","textures":
                [{"user":"texprop","value":"materials/x"}]}]})");
            CHECK(rec.uses.count("texprop") == 1u);
        }
        SUBCASE("a constant bound by name is held by the binding, not recorded here") {
            auto rec = record(R"({"passes":[{"shader":"_t","constantshadervalues":
                {"pulse":{"user":"constprop","value":1}}}]})");
            CHECK(rec.uses.count("constprop") == 0u);
        }
    }

    TEST_CASE("a model submesh material's user constants are live-bound, not static uses") {
        auto parse = [](const char* material) {
            return parseStaticUseScene(
                R"JSON({ "id": 730, "name": "rock", "model": "models/_tri_ub.mdl",
                    "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "visible": true })JSON",
                R"({"modelprop":{"value":1,"type":"slider"}})",
                {
                    { "/models/_tri_ub.mdl", makeTriangleModelMdl("materials/_ub.json") },
                    { "/materials/_ub.json", material },
                });
        };
        SUBCASE("a constant") {
            auto scene = parse(R"({"passes":[{"shader":"_t","blending":"translucent",
                "textures":[],"constantshadervalues":
                {"pulse":{"user":"modelprop","value":1}}}]})");
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropUniformBindings.count("modelprop") == 1u);
            CHECK(scene->userPropStaticUses.count("modelprop") == 0u);
            CHECK(wek::userPropsNeedingReload(*scene, { "modelprop" }).empty());
        }
        SUBCASE("a usershadervalues entry") {
            auto scene = parse(R"({"passes":[{"shader":"_t","blending":"translucent",
                "textures":[],"usershadervalues":{"modelprop":"g_Pulse"}}]})");
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropUniformBindings.count("modelprop") == 1u);
            CHECK(scene->userPropStaticUses.count("modelprop") == 0u);
        }
    }

    TEST_CASE("a property named only in a script's stored scriptproperties is a static use") {
        auto scriptProps = [](const char* name) {
            return std::string(R"JSON("scriptproperties": { "k": { "user": ")JSON") + name +
                   R"JSON(", "value": 1 } })JSON";
        };
        SUBCASE("a color script") {
            auto scene = parseStaticUseScene(
                imageObject("true",
                            R"JSON(, "color": { "script": "function update(v){return v;}",
                                    )JSON" +
                                scriptProps("colorprop") + R"JSON(, "value": "1 1 1" })JSON"),
                R"({"colorprop":{"value":1,"type":"slider"}})");
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropStaticUses.count("colorprop") == 1u);
        }
        SUBCASE("a property script") {
            auto scene = parseStaticUseScene(
                imageObject("true",
                            R"JSON(, "alpha": { "script": "function update(v){return v;}",
                                    )JSON" +
                                scriptProps("alphaprop") + R"JSON(, "value": 1.0 })JSON"),
                R"({"alphaprop":{"value":1,"type":"slider"}})");
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropStaticUses.count("alphaprop") == 1u);
        }
        SUBCASE("an effect's visible script") {
            auto scene =
                parseStaticUseScene(imageObject(
                                        "true",
                                        R"JSON(, "effects": [ { "id": 10, "name": "tint", "visible":
                                { "script": "function update(v){return v;}",
                                )JSON" + scriptProps("fxscriptprop") +
                                            R"JSON(, "value": true },
                                "file": "effects/tint.json" } ])JSON"),
                                    R"({"fxscriptprop":{"value":1,"type":"slider"}})",
                                    { { "/effects/tint.json", kEffectFileJson } });
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropStaticUses.count("fxscriptprop") == 1u);
        }
        SUBCASE("a name only in the script body is left to the sidecar's textual check") {
            auto scene = parseStaticUseScene(imageObject("true",
                                                         R"JSON(, "alpha": { "script":
                                "function update(v){return engine.userProperties.bodyprop;}",
                                "scriptproperties": {}, "value": 1.0 })JSON"),
                                             R"({"bodyprop":{"value":1,"type":"slider"}})");
            REQUIRE(scene != nullptr);
            CHECK(scene->userPropStaticUses.count("bodyprop") == 0u);
        }
    }

    TEST_CASE("a sound object's visible is read but consumed by nothing") {
        wpscene::WPSoundObject snd;
        UserPropUseRecorder    rec;
        auto                   vfs = makeAssetsVfsWith({});
        {
            UserPropUseRecordingScope scope(&rec);
            REQUIRE(snd.FromJson(nlohmann::json::parse(R"({
                "sound": ["a.mp3"], "visible": { "user": "soundvis", "value": true } })"),
                                 *vfs));
        }
        CHECK(rec.uses.count("soundvis") == 0u);
    }

    TEST_CASE("a script-bound effect visible field produces an Effect-attached property script "
              "with the right effectIndex") {
        ensureGlslangInit();
        constexpr const char* kBloomEffectFileJson = R"({
            "name": "bloom",
            "passes": [{ "material": "materials/_plain.json" }]
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
    { "id": 511, "name": "script_bound_effect_layer",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 11, "name": "bloom",
          "visible": {"script": "export function mediaThumbnailChanged(e){}", "value": true},
          "file": "effects/bloom.json" },
        { "id": 10, "name": "tint", "visible": true, "file": "effects/tint.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager   sm;
        WPUserProperties      props {};
        WPSceneParser         parser;
        auto scene = parser.Parse("scene_script_bound_effect", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        int effectScriptCount = 0;
        for (const auto& ps : scene->propertyScripts) {
            if (ps.attachment != ScenePropertyScript::Attachment::Effect) continue;
            effectScriptCount++;
            CHECK(ps.id == 511);
            CHECK(ps.effectIndex == 0); // "bloom" -- "tint" (last) anchors the terminal position
            CHECK(ps.property == "visible");
            CHECK(ps.script == "export function mediaThumbnailChanged(e){}");
        }
        CHECK(effectScriptCount == 1);
    }

    TEST_CASE("a script-bound effect visible field on a single-effect layer's only (and so "
              "terminal) effect now registers at index 0") {
        // The dominant real-world shape (426/865 hits in the library scan):
        // a single-effect chain where the terminal IS effect 0.  CustomShaderPass
        // draws a passthrough pipeline for it when hidden instead of forcing
        // the write visible, so it's safe to register like any other effect.
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/bloom.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 512, "name": "terminal_script_bound_effect",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "bloom",
          "visible": {"script": "export function mediaThumbnailChanged(e){}", "value": true},
          "file": "effects/bloom.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_terminal_script_bound_effect", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        int effectScriptCount = 0;
        for (const auto& ps : scene->propertyScripts) {
            if (ps.attachment != ScenePropertyScript::Attachment::Effect) continue;
            effectScriptCount++;
            CHECK(ps.id == 512);
            CHECK(ps.effectIndex == 0);
        }
        CHECK(effectScriptCount == 1);
    }

    TEST_CASE("a user-bound effect on a single-effect layer's only (and so terminal) effect now "
              "registers at index 0") {
        // The "Blurred Cover" idiom (Now Playing 2883312700, object 2065):
        // one image effect whose own visible field is a plain user-property
        // toggle, not a script.  Same single-effect-chain shape as above, on
        // the user-property path instead of the script one.
        ensureGlslangInit();
        auto                vfs        = makeAssetsVfsWith({
            { "/effects/blur.json", kEffectFileJson },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 2065, "name": "Blurred Cover",
      "image": "models/_plain.json",
      "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 10, "name": "blur",
          "visible": {"user": "blurredcover_enable", "value": true},
          "file": "effects/blur.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto                scene =
            parser.Parse("scene_terminal_user_bound_single_effect", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(2065) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(2065);
        REQUIRE(effLayer->EffectCount() == 1u);

        REQUIRE(scene->userPropEffectVisBindings.count("blurredcover_enable") == 1u);
        const auto& binding = scene->userPropEffectVisBindings.at("blurredcover_enable");
        REQUIRE(binding.size() == 1u);
        CHECK(binding[0].nodeId == 2065);
        CHECK(binding[0].effectIndex == 0);
        CHECK(binding[0].defaultVisible);
    }

    TEST_CASE("Album Cover's real Current/Previous shape resolves both Current and Previous") {
        // Verbatim shape from this wallpaper's real scene.json (Now Playing
        // album-art crossfade): three effects -- a static placeholder
        // ("Defualt"), then two script-gated ones ("Current"/"Previous") that
        // toggle on mediaThumbnailChanged. "Previous" is the structurally-last
        // effect on the layer; CustomShaderPass draws a passthrough pipeline
        // for it when hidden (a Terminal-kind hideable pass) instead of forcing its
        // write visible, so it registers a runtime binding like "Current"
        // does.
        ensureGlslangInit();
        constexpr const char* kDefualtEffectFileJson  = R"({
            "name": "Defualt",
            "passes": [{ "material": "materials/_plain.json" }]
        })";
        constexpr const char* kCurrentEffectFileJson  = R"({
            "name": "Current",
            "passes": [{ "material": "materials/_plain.json" }]
        })";
        constexpr const char* kPreviousEffectFileJson = R"({
            "name": "Previous",
            "passes": [{ "material": "materials/_plain.json" }]
        })";
        auto                  vfs                     = makeAssetsVfsWith({
            { "/effects/Defualt.json", kDefualtEffectFileJson },
            { "/effects/Current.json", kCurrentEffectFileJson },
            { "/effects/Previous.json", kPreviousEffectFileJson },
        });
        const char*           kSceneJson              = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 174, "name": "Album Cover",
      "image": "models/_plain.json",
      "origin": "1100 1080 0", "scale": "1 1 1", "angles": "0 0 0",
      "visible": true,
      "effects": [
        { "id": 473, "name": "Defualt", "visible": true, "file": "effects/Defualt.json" },
        { "id": 476, "name": "Current",
          "visible": {
            "script": "export function mediaThumbnailChanged(event) { thisObject.visible = event.hasThumbnail; }",
            "value": true },
          "file": "effects/Current.json" },
        { "id": 482, "name": "Previous",
          "visible": {
            "script": "export function mediaThumbnailChanged(event) { thisObject.visible = event.hasThumbnail; }",
            "value": true },
          "file": "effects/Previous.json" }
      ] }
  ]
}
)JSON";
        audio::SoundManager   sm;
        WPUserProperties      props {};
        WPSceneParser         parser;
        auto scene = parser.Parse("scene_album_cover", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        REQUIRE(scene->nodeEffectLayerMap.count(174) == 1);
        SceneImageEffectLayer* effLayer = scene->nodeEffectLayerMap.at(174);
        REQUIRE(effLayer->EffectCount() == 3u); // all three build -- none is a plain "false"

        std::vector<std::string> registeredEffects;
        for (const auto& ps : scene->propertyScripts) {
            if (ps.attachment != ScenePropertyScript::Attachment::Effect) continue;
            CHECK(ps.id == 174);
            CHECK(ps.layerName == "Album Cover");
            registeredEffects.push_back(effLayer->GetEffect((std::size_t)ps.effectIndex)->name);
            if (registeredEffects.back() == "Previous") {
                CHECK(ps.effectIndex == 2); // the layer's actual terminal index
            }
        }
        // Both script-gated effects register, including the last one (it
        // draws a passthrough when hidden; see CustomShaderPass::hideable).
        bool sawCurrent = false, sawPrevious = false;
        for (auto& name : registeredEffects) {
            if (name == "Current") sawCurrent = true;
            if (name == "Previous") sawPrevious = true;
        }
        CHECK(registeredEffects.size() == 2u);
        CHECK(sawCurrent);
        CHECK(sawPrevious);
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

    // LoadMaterial's texture loop resolving a "Now Playing" usertextures
    // entry against a material's own authored fallback texture -- the
    // "Album Cover" base-quad shape workshop 3616389236's materials/SS.json
    // uses. Non-square fallback dimensions (96x48) catch a width/height swap
    // a square fixture couldn't.
    TEST_CASE("E2E: a usertextures system entry resolves to the media slot and records the "
              "fallback's real dimensions") {
        ensureGlslangInit();

        // Minimal single-mip RGBA8 .tex (texb=1, no LZ4, no sprite) -- same
        // shape as test_WPTexImageParser.cpp's makeSimpleRGBA8Tex, duplicated
        // locally since that helper isn't header-exported.
        auto appendInt32 = [](std::string& buf, int32_t v) {
            char b[4];
            std::memcpy(b, &v, 4);
            buf.append(b, 4);
        };
        auto appendUint32 = [](std::string& buf, uint32_t v) {
            char b[4];
            std::memcpy(b, &v, 4);
            buf.append(b, 4);
        };
        auto appendTexVersion = [](std::string& buf, int ver) {
            char hdr[9] {};
            std::snprintf(hdr, sizeof(hdr), "%.4s%.4d", "TEX", ver);
            buf.append(hdr, 9);
        };
        auto makeTex = [&](int32_t w, int32_t h) {
            std::string buf;
            appendTexVersion(buf, 1); // TEXV
            appendTexVersion(buf, 1); // TEXI
            appendInt32(buf, 0);      // type = RGBA8
            appendUint32(buf, 0);     // flags
            appendInt32(buf, w);
            appendInt32(buf, h);
            appendInt32(buf, w);      // mapWidth
            appendInt32(buf, h);      // mapHeight
            appendInt32(buf, 0);      // unknown
            appendTexVersion(buf, 1); // TEXB
            appendInt32(buf, 1);      // image count
            appendInt32(buf, 1);      // mipmap count
            appendInt32(buf, w);
            appendInt32(buf, h);
            std::string pixels((size_t)(w * h * 4), '\xAB');
            appendInt32(buf, (int32_t)pixels.size());
            buf += pixels;
            return buf;
        };

        // A frag unit that DECLARES g_Texture0 (without sampling it) so the
        // shader preprocessor's active_tex_slots regex keeps slot 0 --
        // WPSceneParser clears any material texture slot the shader doesn't
        // declare, which would otherwise erase the resolved "$mediaThumbnail"
        // name before this test ever sees it (see test_WarmCacheTexSlots.cpp
        // for the same idiom).
        const char* kCoverFrag = R"GLSL(
uniform sampler2D g_Texture0;
void main() {
    gl_FragColor = vec4(1.0);
}
)GLSL";

        auto                vfs        = makeAssetsVfsWith({
            { "/materials/SS.tex", makeTex(96, 48) },
            { "/shaders/_cover.vert", kTrivialVert },
            { "/shaders/_cover.frag", kCoverFrag },
            { "/models/_cover.json", R"({"material": "materials/_cover.json"})" },
            { "/materials/_cover.json",
              R"({"passes": [{"shader": "_cover", "blending": "translucent",
                    "textures": ["SS"],
                    "usertextures": [{"name": "$mediaThumbnail", "type": "system"}]}]})" },
        });
        const char*         kSceneJson = R"JSON(
{
  "general": { "clearcolor": "0 0 0",
               "orthogonalprojection": { "width": 1280, "height": 720 } },
  "objects": [
    { "id": 174, "name": "Album Cover", "image": "models/_cover.json",
      "origin": "1100 1080 0", "scale": "1 1 1", "angles": "0 0 0",
      "size": "1410 1410", "visible": true }
  ]
}
)JSON";
        audio::SoundManager sm;
        WPUserProperties    props {};
        WPSceneParser       parser;
        auto scene = parser.Parse("scene_media_usertexture", kSceneJson, *vfs, sm, props);
        REQUIRE(scene != nullptr);

        auto it = scene->nodeById.find(174);
        REQUIRE(it != scene->nodeById.end());
        REQUIRE(it->second->Mesh() != nullptr);
        REQUIRE(it->second->Mesh()->Material() != nullptr);
        const auto& textures = it->second->Mesh()->Material()->textures;
        REQUIRE(textures.size() == 1);
        CHECK(textures[0] == "$mediaThumbnail");

        REQUIRE(scene->mediaTextureSlots.size() == 1);
        CHECK(scene->mediaTextureSlots[0].textureKey == "$mediaThumbnail");
        CHECK(scene->mediaTextureSlots[0].width == 96);
        CHECK(scene->mediaTextureSlots[0].height == 48);

        // "SS" must still resolve to itself for any other, non-system reference.
        auto ss = scene->imageParser->Parse("SS");
        REQUIRE(ss);
        CHECK(ss->key == "SS");
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

namespace
{
constexpr const char* kUbFrag         = R"GLSL(
uniform float g_Pulse; // {"material":"pulse","default":1}
void main() {
    gl_FragColor = vec4(g_Pulse);
}
)GLSL";
constexpr const char* kUbMaterialJson = R"({
    "passes": [{ "shader": "_ub", "blending": "normal", "textures": [],
                 "constantshadervalues": { "pulse": { "user": "myprop", "value": 1 } } }]
})";
// The 3662790108 太空球 shape: the user reference sits inside a script's
// scriptproperties, not at the constant's top level.
constexpr const char* kUbScriptedMaterialJson = R"({
    "passes": [{ "shader": "_ub", "blending": "normal", "textures": [],
                 "constantshadervalues": { "pulse": {
                     "script": "export function update(value) { return value; }",
                     "scriptproperties": { "starb": { "user": "myprop", "value": 1 } },
                     "value": 1 } } }]
})";
constexpr const char* kUbBrokenMaterialJson   = R"({
    "passes": [{ "shader": "broken", "blending": "normal", "textures": [],
                 "constantshadervalues": { "pulse": { "user": "myprop", "value": 1 } } }]
})";

std::unique_ptr<fs::VFS>
makeUbVfs(std::initializer_list<std::pair<std::string, std::string>> more) {
    auto vfs   = std::make_unique<fs::VFS>();
    auto memfs = std::make_unique<MemFs>();
    memfs->add("/shaders/_t.vert", kTrivialVert);
    memfs->add("/shaders/_t.frag", kTrivialFrag);
    memfs->add("/shaders/_ub.vert", kTrivialVert);
    memfs->add("/shaders/_ub.frag", kUbFrag);
    memfs->add("/materials/_plain.json", kPlainMaterialJson);
    memfs->add("/materials/_ub.json", kUbMaterialJson);
    memfs->add("/materials/_ub_scripted.json", kUbScriptedMaterialJson);
    memfs->add("/materials/_ub_broken.json", kUbBrokenMaterialJson);
    memfs->add("/models/_ub_img_plain.json",
               R"({"material":"materials/_plain.json","width":256,"height":256})");
    memfs->add("/models/_ub_img.json",
               R"({"material":"materials/_ub.json","width":256,"height":256})");
    for (const auto& [p, c] : more) memfs->add(p, c);
    REQUIRE(vfs->Mount("/assets", std::move(memfs)));
    return vfs;
}

std::shared_ptr<Scene> parseUb(fs::VFS& vfs, const std::string& objectJson) {
    std::string         scene = R"({"general":{"clearcolor":"0 0 0",
        "orthogonalprojection":{"width":1280,"height":720}},"objects":[)" +
                                objectJson + "]}";
    audio::SoundManager sm;
    WPUserProperties    props {};
    WPSceneParser       parser;
    return parser.Parse("scene_ub", scene, vfs, sm, props);
}
} // namespace

TEST_SUITE("WPSceneParser user-property uniform bindings on model and particle materials") {
    TEST_CASE("control: an image layer with the same material registers the binding") {
        ensureGlslangInit();
        auto vfs   = makeUbVfs({});
        auto scene = parseUb(*vfs, R"({"id":10,"name":"img","image":"models/_ub_img.json",
            "origin":"0 0 0","scale":"1 1 1","angles":"0 0 0","visible":true})");
        REQUIRE(scene != nullptr);
        REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
        CHECK(scene->userPropUniformBindings.at("myprop")[0].uniformName == "g_Pulse");
    }

    TEST_CASE("model submesh material with a user constant registers a uniform binding") {
        ensureGlslangInit();
        const char* kModelObj = R"({"id":501,"name":"m","model":"models/m.mdl",
            "origin":"0 0 0","scale":"1 1 1","angles":"0 0 0","visible":true})";

        SUBCASE("one submesh: bound to that submesh's mesh, aliased uniform name") {
            auto vfs = makeUbVfs({ { "/models/m.mdl", makeModelMdlN({ "materials/_ub.json" }) } });
            auto scene = parseUb(*vfs, kModelObj);
            REQUIRE(scene != nullptr);
            REQUIRE(scene->nodeById.count(501) == 1);
            auto& kids = scene->nodeById.at(501)->GetChildren();
            REQUIRE(kids.size() == 1u);
            SceneMesh* subMesh = kids.front()->Mesh();
            REQUIRE(subMesh != nullptr);
            REQUIRE(subMesh->Material() != nullptr); // material really loaded

            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            const auto& bs = scene->userPropUniformBindings.at("myprop");
            REQUIRE(bs.size() == 1u);
            CHECK(bs[0].mesh.get() == subMesh);
            CHECK(bs[0].uniformName == "g_Pulse");
        }

        SUBCASE("two submeshes: one binding per submesh mesh") {
            auto vfs =
                makeUbVfs({ { "/models/m.mdl",
                              makeModelMdlN({ "materials/_ub.json", "materials/_ub.json" }) } });
            auto scene = parseUb(*vfs, kModelObj);
            REQUIRE(scene != nullptr);
            auto& kids = scene->nodeById.at(501)->GetChildren();
            REQUIRE(kids.size() == 2u);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            const auto& bs = scene->userPropUniformBindings.at("myprop");
            REQUIRE(bs.size() == 2u);
            std::vector<SceneMesh*> got { bs[0].mesh.get(), bs[1].mesh.get() };
            for (auto& k : kids) CHECK(std::find(got.begin(), got.end(), k->Mesh()) != got.end());
        }

        SUBCASE("a submesh whose material fails to load leaves no binding") {
            auto vfs =
                makeUbVfs({ { "/models/m.mdl", makeModelMdlN({ "materials/_ub_broken.json" }) } });
            auto scene = parseUb(*vfs, kModelObj);
            REQUIRE(scene != nullptr);
            CHECK(scene->nodeById.at(501)->GetChildren().empty());
            CHECK(scene->userPropUniformBindings.count("myprop") == 0u);
        }

        SUBCASE("a user reference inside scriptproperties is not a uniform binding") {
            auto vfs = makeUbVfs(
                { { "/models/m.mdl", makeModelMdlN({ "materials/_ub_scripted.json" }) } });
            auto scene = parseUb(*vfs, kModelObj);
            REQUIRE(scene != nullptr);
            auto& kids = scene->nodeById.at(501)->GetChildren();
            REQUIRE(kids.size() == 1u);
            REQUIRE(kids.front()->Mesh()->Material() != nullptr);
            CHECK(scene->userPropUniformBindings.count("myprop") == 0u);
        }
    }

    TEST_CASE("particle material with a user constant registers a uniform binding") {
        ensureGlslangInit();
        const char* kParticleObj = R"({"id":600,"name":"p","particle":"particle/p.json",
            "origin":"0 0 0","scale":"1 1 1","angles":"0 0 0","visible":true})";

        SUBCASE("system mesh") {
            auto vfs   = makeUbVfs({ { "/particle/p.json",
                                       R"({"emitter":[{}],"renderer":[{"name":"sprite"}],
                                         "maxcount":4,"material":"materials/_ub.json"})" } });
            auto scene = parseUb(*vfs, kParticleObj);
            REQUIRE(scene != nullptr);
            REQUIRE(scene->nodeById.count(600) == 1);
            SceneMesh* mesh = scene->nodeById.at(600)->Mesh();
            REQUIRE(mesh != nullptr);
            REQUIRE(mesh->Material() != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            const auto& bs = scene->userPropUniformBindings.at("myprop");
            REQUIRE(bs.size() == 1u);
            CHECK(bs[0].mesh.get() == mesh);
            CHECK(bs[0].uniformName == "g_Pulse");
        }

        SUBCASE("a child system registers against its own mesh") {
            auto vfs   = makeUbVfs({
                { "/particle/p.json",
                  R"({"emitter":[{}],"renderer":[{"name":"sprite"}],"maxcount":4,
                      "material":"materials/_ub.json",
                      "children":[{"name":"particle/c.json"}]})" },
                { "/particle/c.json",
                  R"({"emitter":[{}],"renderer":[{"name":"sprite"}],"maxcount":4,
                      "material":"materials/_ub.json"})" },
            });
            auto scene = parseUb(*vfs, kParticleObj);
            REQUIRE(scene != nullptr);
            SceneNode* sys = scene->nodeById.at(600);
            REQUIRE(sys->Mesh() != nullptr);
            REQUIRE(sys->Mesh()->Material() != nullptr);
            REQUIRE(sys->GetChildren().size() == 1u);
            SceneMesh* childMesh = sys->GetChildren().front()->Mesh();
            REQUIRE(childMesh != nullptr);
            REQUIRE(childMesh->Material() != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            const auto& bs = scene->userPropUniformBindings.at("myprop");
            REQUIRE(bs.size() == 2u);
            std::vector<SceneMesh*> got { bs[0].mesh.get(), bs[1].mesh.get() };
            CHECK(std::find(got.begin(), got.end(), sys->Mesh()) != got.end());
            CHECK(std::find(got.begin(), got.end(), childMesh) != got.end());
        }
    }

    TEST_CASE("a property read by a model material and by an object color still reloads") {
        ensureGlslangInit();
        auto vfs = makeUbVfs({ { "/models/m.mdl", makeModelMdlN({ "materials/_ub.json" }) } });

        SUBCASE("model material and image color share the property") {
            auto scene = parseUb(*vfs, R"({"id":501,"name":"m","model":"models/m.mdl",
                "origin":"0 0 0","scale":"1 1 1","angles":"0 0 0","visible":true},
                {"id":10,"name":"img","image":"models/_ub_img_plain.json",
                "color":{"user":"myprop","value":"1 0 0"},
                "origin":"0 0 0","scale":"1 1 1","angles":"0 0 0","visible":true})");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            CHECK(wek::userPropsNeedingReload(*scene, { "myprop" }) ==
                  std::vector<std::string> { "myprop" });
        }

        SUBCASE("a property only a model material reads applies live") {
            auto scene = parseUb(*vfs, R"({"id":501,"name":"m","model":"models/m.mdl",
                "origin":"0 0 0","scale":"1 1 1","angles":"0 0 0","visible":true})");
            REQUIRE(scene != nullptr);
            REQUIRE(scene->userPropUniformBindings.count("myprop") == 1u);
            CHECK(wek::userPropsNeedingReload(*scene, { "myprop" }).empty());
        }
    }
}
