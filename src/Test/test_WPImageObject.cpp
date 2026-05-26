#include <doctest.h>

#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"
#include "wpscene/WPImageObject.h"
#include "Scene/SceneImageEffectLayer.h"
#include "Scene/SceneNode.h"

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;

namespace
{

// In-memory VFS that serves model + material JSON content for the wpscene
// parser.  WPImageObject::FromJson resolves "/assets/<image>" so each test
// pre-loads the model JSON path and (when the model points at a material) the
// referenced material JSON too.
class MemFs : public Fs {
public:
    void add(std::string path, std::string content) {
        std::vector<uint8_t> bytes(content.begin(), content.end());
        m_files[std::move(path)] = std::move(bytes);
    }
    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) > 0;
    }
    std::shared_ptr<IBinaryStream> Open(std::string_view path) override {
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        auto copy = it->second;
        return std::make_shared<MemBinaryStream>(std::move(copy));
    }
    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view) override { return nullptr; }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

// VFS is NoCopy/NoMove — return by unique_ptr so test cases can pass it around.
//
// Caller passes paths *relative* to the /assets mount (matching how
// WPImageObject's `image` and `material` fields are stored in scene.json) —
// e.g. "models/util/solidlayer.json".  The helper prepends the leading slash
// the VFS expects after stripping the mount prefix.
std::unique_ptr<VFS> makeAssetsVfs(const std::unordered_map<std::string, std::string>& files) {
    auto vfs = std::make_unique<VFS>();
    auto fs  = std::make_unique<MemFs>();
    for (const auto& [path, content] : files) fs->add("/" + path, content);
    REQUIRE(vfs->Mount("/assets", std::move(fs)));
    return vfs;
}

// Stock material JSON used by every test; image/composelayer/solidlayer all
// point at a real shader so material parsing succeeds.
constexpr const char* kFlatMaterial = R"({
    "passes": [{ "shader": "flat", "blending": "translucent" }]
})";

} // namespace

TEST_SUITE("WPImageObject parsing — gap fixes") {
    // ---- gap 1: solidlayer flag from model JSON ----------------------------
    TEST_CASE("model JSON solidlayer:true populates WPImageObject.solidlayer") {
        auto vfs       = makeAssetsVfs({
            { "models/util/solidlayer.json",
                    R"({ "material": "materials/util/solidlayer.json", "solidlayer": true })" },
            { "materials/util/solidlayer.json", kFlatMaterial },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 42, "name": "audio_bars", "image": "models/util/solidlayer.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "512 512"
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        CHECK(obj.solidlayer == true);
    }

    TEST_CASE("model JSON without solidlayer leaves the flag at default false") {
        auto vfs       = makeAssetsVfs({
            { "models/regular.json", R"({ "material": "materials/util/solidlayer.json" })" },
            { "materials/util/solidlayer.json", kFlatMaterial },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 1, "name": "x", "image": "models/regular.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "100 100"
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        CHECK(obj.solidlayer == false);
    }

    // ---- gap 5: copybackground flag from scene.json ------------------------
    TEST_CASE("scene.json copybackground:false populates WPImageObject.copybackground") {
        auto vfs       = makeAssetsVfs({
            { "models/util/composelayer.json",
                    R"({ "material": "materials/util/composelayer.json", "passthrough": true })" },
            { "materials/util/composelayer.json", kFlatMaterial },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 7, "name": "compose", "image": "models/util/composelayer.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "1500 1500",
            "copybackground": false
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        CHECK(obj.copybackground == false);
    }

    TEST_CASE("scene.json copybackground:true populates WPImageObject.copybackground") {
        auto vfs       = makeAssetsVfs({
            { "models/util/composelayer.json",
                    R"({ "material": "materials/util/composelayer.json", "passthrough": true })" },
            { "materials/util/composelayer.json", kFlatMaterial },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 7, "name": "compose", "image": "models/util/composelayer.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "1500 1500",
            "copybackground": true
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        CHECK(obj.copybackground == true);
    }

    TEST_CASE("scene.json without copybackground defaults to true") {
        auto vfs       = makeAssetsVfs({
            { "models/util/composelayer.json",
                    R"({ "material": "materials/util/composelayer.json" })" },
            { "materials/util/composelayer.json", kFlatMaterial },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 7, "name": "compose", "image": "models/util/composelayer.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "1500 1500"
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        CHECK(obj.copybackground == true);
    }

    // ---- gap 1+3 are independent: solidlayer flag is parsed even when ------
    //       the scene has alpha=1 explicitly.  WPSceneParser then consults
    //       solidlayer to override g_Alpha=0 — that override is verified in
    //       a separate end-to-end render test on Nightingale 3470764447.
    TEST_CASE("solidlayer flag survives alongside explicit alpha=1") {
        auto vfs       = makeAssetsVfs({
            { "models/util/solidlayer.json",
                    R"({ "material": "materials/util/solidlayer.json", "solidlayer": true })" },
            { "materials/util/solidlayer.json", kFlatMaterial },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 42, "name": "x", "image": "models/util/solidlayer.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "100 100",
            "alpha": 1.0
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        CHECK(obj.solidlayer == true);
        CHECK(obj.alpha == doctest::Approx(1.0f));
    }

    // ---- compose-layer `dependencies` parsing ------------------------------
    // Compose layers (and the dependents they reference) drive the
    // "image referenced by another image" offscreen-routing rule in
    // WPSceneParser.  Without this list, the dependent images render to
    // _rt_default and the compose blend reads a full-FB snapshot via
    // _rt_link_<id> — producing solid gray quads over each character
    // (Clair Obscur Expedition 33 3498984739).
    TEST_CASE("compose layer dependencies array parses into WPImageObject.dependencies") {
        auto vfs       = makeAssetsVfs({
            { "models/util/composelayer.json",
                    R"({ "material": "materials/util/composelayer.json" })" },
            { "materials/util/composelayer.json", kFlatMaterial },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 413, "name": "Calque CO33-M3", "image": "models/util/composelayer.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "500 500",
            "dependencies": [484, 555, 625]
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        REQUIRE(obj.dependencies.size() == 3);
        CHECK(obj.dependencies[0] == 484);
        CHECK(obj.dependencies[1] == 555);
        CHECK(obj.dependencies[2] == 625);
    }

    TEST_CASE("scene.json without dependencies leaves WPImageObject.dependencies empty") {
        auto vfs       = makeAssetsVfs({
            { "models/util/composelayer.json",
                    R"({ "material": "materials/util/composelayer.json" })" },
            { "materials/util/composelayer.json", kFlatMaterial },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 7, "name": "plain", "image": "models/util/composelayer.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "500 500"
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        CHECK(obj.dependencies.empty());
    }

    TEST_CASE("shape-quad path also accepts dependencies array") {
        // Shape-quad objects (no `image` field) follow a separate JSON branch
        // in WPImageObject::FromJson — pin parity so a future shape-quad
        // compose layer can also force dependents offscreen.
        auto vfs       = makeAssetsVfs({});
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 99, "name": "shapeq", "shape": "quad",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "100 100",
            "dependencies": [10, 20]
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        REQUIRE(obj.dependencies.size() == 2);
        CHECK(obj.dependencies[0] == 10);
        CHECK(obj.dependencies[1] == 20);
    }

    // ---- shape-quad blend override for DIRECTDRAW effects ------------------
    // Shape-quad objects host procedural effect shaders (lightshafts, lensflare,
    // motionblur).  When an effect pass authors DIRECTDRAW=1, the shader does
    // `albedo = vec4(0)` and emits premultiplied RGB (`fxColor * intensity * fx`)
    // alongside `alpha = fx`.  The default Translucent blend then re-multiplies
    // by alpha producing `fx² * fxColor + (1-fx) * dst` — rays appear as dim
    // dark stripes that *darken* the background instead of brightening it.
    // Force additive blending on shape-quads whose effects opt into DIRECTDRAW.
    // Driver: Glowing Girl 4K (3287715210) "光束 - 径向" lightshafts pass.
    TEST_CASE("shape-quad with DIRECTDRAW=1 effect pass forces additive blend") {
        auto vfs       = makeAssetsVfs({
            { "effects/lightshafts/effect.json",
              R"({
                "passes": [{ "material": "materials/effects/lightshafts.json" }]
            })" },
            { "materials/effects/lightshafts.json",
              R"({
                "passes": [{ "shader": "effects/lightshafts", "blending": "normal" }]
            })" },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 470, "name": "rays", "shape": "quad",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "100 100",
            "effects": [{
                "file": "effects/lightshafts/effect.json",
                "id": 471, "visible": true,
                "passes": [{ "combos": { "DIRECTDRAW": 1, "RAYMODE": 1, "RENDERING": 1 } }]
            }]
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        CHECK(obj.material.blending == "additive");
    }

    TEST_CASE("shape-quad with DIRECTDRAW=0 effect pass keeps default translucent blend") {
        // Counter-pin: only DIRECTDRAW=1 triggers the override — DIRECTDRAW=0
        // shaders read g_Texture0 and composite internally with mix(), so the
        // hardware Translucent path is correct for them.
        auto vfs       = makeAssetsVfs({
            { "effects/foo/effect.json",
              R"({
                "passes": [{ "material": "materials/effects/foo.json" }]
            })" },
            { "materials/effects/foo.json",
              R"({
                "passes": [{ "shader": "effects/foo", "blending": "normal" }]
            })" },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 470, "name": "fx", "shape": "quad",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "100 100",
            "effects": [{
                "file": "effects/foo/effect.json",
                "id": 471, "visible": true,
                "passes": [{ "combos": { "DIRECTDRAW": 0 } }]
            }]
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        CHECK(obj.material.blending == "translucent");
    }

    TEST_CASE("shape-quad without effects keeps default translucent blend") {
        // No effects at all → no DIRECTDRAW combo to inspect → keep default.
        auto vfs       = makeAssetsVfs({});
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 99, "name": "plain-quad", "shape": "quad",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "100 100"
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        CHECK(obj.material.blending == "translucent");
    }

    // ---- CollectComposeDependencyIds: pre-pass filter --------------------
    // The WPSceneParser pre-pass that forces dependent images offscreen
    // consumes only *compose-layer* dependencies and ignores self-references.
    // Two driver wallpapers anchor these filters:
    //
    //   - Clair Obscur Expedition 33 3498984739 — compose layers Calque
    //     CO33-Mx declare body-part image ids in `dependencies`; those ids
    //     MUST be routed offscreen so the compose blend samples a sprite RT
    //     instead of a full-FB snapshot.
    //   - Eclipse 1210462523 — a single non-compose image layer (id 64,
    //     `image: models/868850.json`) declared `dependencies:[64,64,64]`.
    //     If those self-refs were collected, the layer was forced to
    //     `_rt_offscreen_64`, nothing read it, and the screen stayed at
    //     clearColor (totally blank wallpaper on the desktop).
    TEST_CASE("CollectComposeDependencyIds: compose layer's dependencies are collected") {
        wpscene::WPImageObject compose;
        compose.id           = 100;
        compose.image        = "models/util/composelayer.json";
        compose.dependencies = { 484, 555, 625 };

        auto ids = wpscene::CollectComposeDependencyIds({ &compose });
        CHECK(ids.size() == 3);
        CHECK(ids.count(484) == 1);
        CHECK(ids.count(555) == 1);
        CHECK(ids.count(625) == 1);
    }

    TEST_CASE("CollectComposeDependencyIds: non-compose layer's dependencies are IGNORED") {
        // Eclipse 1210462523: the only layer is a plain image (not a compose
        // layer) and lists its own id as a dependency.  Forcing that layer
        // offscreen produces a blank wallpaper because no compose blend
        // ever samples the offscreen RT.
        wpscene::WPImageObject plain;
        plain.id           = 64;
        plain.image        = "models/868850.json";
        plain.dependencies = { 64, 64, 64 };

        auto ids = wpscene::CollectComposeDependencyIds({ &plain });
        CHECK(ids.empty());
    }

    TEST_CASE("CollectComposeDependencyIds: self-reference on a compose layer is dropped") {
        // Even if a compose layer declares its own id in `dependencies`,
        // that's circular (you cannot sample your own output via a link RT)
        // and must be filtered.
        wpscene::WPImageObject compose;
        compose.id           = 200;
        compose.image        = "models/util/composelayer.json";
        compose.dependencies = { 200, 300, 200, 400 };

        auto ids = wpscene::CollectComposeDependencyIds({ &compose });
        CHECK(ids.size() == 2);
        CHECK(ids.count(200) == 0);
        CHECK(ids.count(300) == 1);
        CHECK(ids.count(400) == 1);
    }

    TEST_CASE("CollectComposeDependencyIds: duplicates across layers collapse to one id") {
        wpscene::WPImageObject c1, c2;
        c1.id           = 10;
        c1.image        = "models/util/composelayer.json";
        c1.dependencies = { 1, 2 };
        c2.id           = 20;
        c2.image        = "models/util/composelayer.json";
        c2.dependencies = { 2, 3 };

        auto ids = wpscene::CollectComposeDependencyIds({ &c1, &c2 });
        CHECK(ids.size() == 3);
        CHECK(ids.count(1) == 1);
        CHECK(ids.count(2) == 1);
        CHECK(ids.count(3) == 1);
    }

    TEST_CASE("CollectComposeDependencyIds: mixed compose / non-compose only honors compose") {
        wpscene::WPImageObject compose, plain;
        compose.id           = 1;
        compose.image        = "models/util/composelayer.json";
        compose.dependencies = { 100, 200 };
        plain.id             = 2;
        plain.image          = "models/something_else.json";
        plain.dependencies   = { 300, 400 }; // ignored

        auto ids = wpscene::CollectComposeDependencyIds({ &compose, &plain });
        CHECK(ids.size() == 2);
        CHECK(ids.count(100) == 1);
        CHECK(ids.count(200) == 1);
        CHECK(ids.count(300) == 0);
        CHECK(ids.count(400) == 0);
    }

    TEST_CASE("CollectComposeDependencyIds: empty / nullptr inputs are handled") {
        CHECK(wpscene::CollectComposeDependencyIds({}).empty());

        // nullptr entries are tolerated.
        wpscene::WPImageObject compose;
        compose.id           = 5;
        compose.image        = "models/util/composelayer.json";
        compose.dependencies = { 99 };
        auto ids             = wpscene::CollectComposeDependencyIds({ nullptr, &compose, nullptr });
        CHECK(ids.size() == 1);
        CHECK(ids.count(99) == 1);
    }

    TEST_CASE("dependencies tolerates non-integer entries (skipped, not crash)") {
        // Hardened against author/serialiser quirks where a dependencies entry
        // is a string or boolean: skip non-integers silently rather than
        // throwing inside the JSON visitor.
        auto vfs       = makeAssetsVfs({
            { "models/util/composelayer.json",
                    R"({ "material": "materials/util/composelayer.json" })" },
            { "materials/util/composelayer.json", kFlatMaterial },
        });
        auto sceneJson = nlohmann::json::parse(R"({
            "id": 1, "name": "z", "image": "models/util/composelayer.json",
            "origin": "0 0 0", "scale": "1 1 1", "angles": "0 0 0", "size": "100 100",
            "dependencies": [123, "oops", null, 456, true]
        })");

        wpscene::WPImageObject obj;
        REQUIRE(obj.FromJson(sceneJson, *vfs));
        REQUIRE(obj.dependencies.size() == 2);
        CHECK(obj.dependencies[0] == 123);
        CHECK(obj.dependencies[1] == 456);
    }
}

TEST_SUITE("SceneImageEffectLayer — gap fixes") {
    TEST_CASE("CopyBackground defaults to true and round-trips through setter") {
        SceneNode             placeholder;
        SceneImageEffectLayer layer(&placeholder, 100.f, 100.f, "ppA", "ppB");
        CHECK(layer.CopyBackground() == true); // default
        layer.SetCopyBackground(false);
        CHECK(layer.CopyBackground() == false);
        layer.SetCopyBackground(true);
        CHECK(layer.CopyBackground() == true);
    }

    TEST_CASE("CopyBackground is independent of Passthrough flag") {
        SceneNode             placeholder;
        SceneImageEffectLayer layer(&placeholder, 100.f, 100.f, "ppA", "ppB");
        layer.SetPassthrough(true);
        layer.SetCopyBackground(false);
        CHECK(layer.IsPassthrough() == true);
        CHECK(layer.CopyBackground() == false);
    }

    // SceneToRenderGraph.cpp::ToGraphPass applies these rules for compose
    // layer base-pass emission based on the IsPassthrough / CopyBackground
    // flag combination.  Pinning the contract here so future changes don't
    // silently regress.
    //
    // Long Train (1457581889) godrays compose layer at origin (764.9, 506.1)
    // — shifted ~196px left of scene center — used to ghost the character
    // (drawn earlier in the frame) onto the left of the screen because the
    // passthrough+copybackground=true branch emitted a plain Copy that
    // captured the framebuffer un-shifted; the final draw then composited
    // scene-center content at the layer's offset position.  Fix: the
    // passthrough+copybackground=true branch now falls through to the
    // normal node-material pass, which runs the composelayer shader and
    // applies the layer's world transform to UV sampling (matching the
    // non-passthrough compose path and WE's behavior).
    TEST_CASE("passthrough+copybackground flag combinations — base-pass emission contract") {
        SceneNode             placeholder;
        SceneImageEffectLayer layer(&placeholder, 100.f, 100.f, "ppA", "ppB");

        SUBCASE("non-passthrough: base pass runs (default)") {
            // SceneToRenderGraph: normal CustomShaderPass for node->material.
            CHECK(layer.IsPassthrough() == false);
            CHECK(layer.CopyBackground() == true);
        }

        SUBCASE("passthrough + copybackground=true: base pass runs (composelayer shader)") {
            layer.SetPassthrough(true);
            // SceneToRenderGraph: falls through to normal CustomShaderPass.
            // The composelayer.vert/.frag applies the layer's MVP to UV
            // sampling, producing a shifted pingpong that aligns with the
            // final draw at the layer's screen position.
            CHECK(layer.IsPassthrough() == true);
            CHECK(layer.CopyBackground() == true);
        }

        SUBCASE("passthrough + copybackground=false: no base pass, no copy") {
            layer.SetPassthrough(true);
            layer.SetCopyBackground(false);
            // SceneToRenderGraph: skip both base pass AND the implicit copy.
            // Author populates the pingpong via child scene-graph passes
            // (Nightingale 3470764447 BlendReflect).
            CHECK(layer.IsPassthrough() == true);
            CHECK(layer.CopyBackground() == false);
        }
    }
}
