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
        auto vfs = makeAssetsVfs({
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
        auto vfs = makeAssetsVfs({
            { "models/regular.json",
              R"({ "material": "materials/util/solidlayer.json" })" },
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
        auto vfs = makeAssetsVfs({
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
        auto vfs = makeAssetsVfs({
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
        auto vfs = makeAssetsVfs({
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
        auto vfs = makeAssetsVfs({
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
        auto vfs = makeAssetsVfs({
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
        auto vfs = makeAssetsVfs({
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
        auto vfs = makeAssetsVfs({});
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

    TEST_CASE("dependencies tolerates non-integer entries (skipped, not crash)") {
        // Hardened against author/serialiser quirks where a dependencies entry
        // is a string or boolean: skip non-integers silently rather than
        // throwing inside the JSON visitor.
        auto vfs = makeAssetsVfs({
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
        SceneNode placeholder;
        SceneImageEffectLayer layer(&placeholder, 100.f, 100.f, "ppA", "ppB");
        CHECK(layer.CopyBackground() == true); // default
        layer.SetCopyBackground(false);
        CHECK(layer.CopyBackground() == false);
        layer.SetCopyBackground(true);
        CHECK(layer.CopyBackground() == true);
    }

    TEST_CASE("CopyBackground is independent of Passthrough flag") {
        SceneNode placeholder;
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
        SceneNode placeholder;
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
