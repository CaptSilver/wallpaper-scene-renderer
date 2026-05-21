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
#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneLight.hpp"
#include "Scene/SceneNode.h"
#include "WPUserProperties.hpp"

#include "Audio/SoundManager.h"

#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
        // PARSE_JSON fails → Parse bails at the very top with nullptr.  Use a
        // hard syntax error that survives the trailing-comma/leading-zero
        // recovery passes (an unterminated object).
        auto scene = parser.Parse("bad", "{ \"general\": ", *vfs, sm, props);
        CHECK(scene == nullptr);
    }

} // TEST_SUITE
