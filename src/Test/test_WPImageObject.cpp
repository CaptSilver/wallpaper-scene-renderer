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
}
