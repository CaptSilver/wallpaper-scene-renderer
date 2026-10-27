#pragma once

// Fixtures shared by the end-to-end WPSceneParser::Parse() suites: an
// in-memory /assets filesystem preloaded with a trivial shader pair and a plain
// image/material/effect, plus the log capture and glslang init those suites
// need.  Everything is inline in wek::test so several translation units of
// backend_scene_tests can include it without ODR clashes.

#include <doctest.h>

#include "WPShaderParser.hpp"
#include "Utils/Logging.h"

#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wek::test
{

using namespace wallpaper;

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

// Process-wide glslang init.  CompileToSpv -> CompileShaderUnits needs the
// glslang globals.  Mirrors the std::call_once pattern in
// test_WarmCacheTexSlots.cpp + test_thread_repro.cpp; never paired with a
// FinalGlslang() — process exit handles teardown.
inline void ensureGlslangInit() {
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
inline constexpr const char* kTrivialVert = R"GLSL(
attribute vec3 a_Position;
void main() {
    gl_Position = vec4(a_Position, 1.0);
}
)GLSL";

inline constexpr const char* kTrivialFrag = R"GLSL(
void main() {
    gl_FragColor = vec4(1.0);
}
)GLSL";

// Minimal `passes`-wrapped material JSON pointing at the trivial shader.
// WPMaterial::FromJson requires the outer `{ "passes": [ { "shader": "..."
// } ] }` shape (see wpscene/WPMaterial.cpp:84-93).
inline constexpr const char* kPlainMaterialJson = R"({
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
inline constexpr const char* kPlainImageJson = R"({
    "material": "materials/_plain.json",
    "width": 256,
    "height": 256
})";

// Effect file referenced by the headline (a-2) case.  WPImageEffect::
// FromFileJson requires `name` + `passes` with each pass containing a
// `material` path (wpscene/WPImageObject.cpp:267-323).  The lone pass
// points at the same trivial-shader material so the effect's glslang
// compile succeeds.
inline constexpr const char* kEffectFileJson = R"({
    "name": "tint",
    "passes": [{
        "material": "materials/_plain.json"
    }]
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
inline std::unique_ptr<fs::VFS>
makeAssetsVfsFrom(const std::vector<std::pair<std::string, std::string>>& extras) {
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

inline std::unique_ptr<fs::VFS>
makeAssetsVfsWith(std::initializer_list<std::pair<std::string, std::string>> extras) {
    return makeAssetsVfsFrom({ extras.begin(), extras.end() });
}

// Fragment shader declaring two samplers and g_Color4.  The parser clears any
// material texture slot its shader does not declare, so a fixture that needs
// slot 0 (chain input) and slot 1 (a composite read) to survive uses this.
inline constexpr const char* kTwoSamplerFrag = R"GLSL(
uniform sampler2D g_Texture0;
uniform sampler2D g_Texture1;
uniform vec4 g_Color4;
void main() {
    gl_FragColor = g_Color4;
}
)GLSL";

// Fragment shader for a puppet composite: the three uniforms a WE image shader
// multiplies into its output, so a fixture can see whether each is neutral.
inline constexpr const char* kImageTintFrag = R"GLSL(
uniform sampler2D g_Texture0;
uniform vec4 g_Color4;
uniform float g_UserAlpha;
uniform float g_Brightness;
void main() {
    gl_FragColor = g_Color4 * g_UserAlpha * g_Brightness;
}
)GLSL";

// VFS for the isolated-link cases: a "tint" effect and the stock
// effectpassthrough material, both on the two-sampler shader, plus the
// compose-layer model, a fullscreen image model, an additive image model and
// a plain image on the colour/alpha-declaring shader.  `extras` layer on top.
inline std::unique_ptr<fs::VFS>
makeIsolatedLinkVfs(const std::vector<std::pair<std::string, std::string>>& extras = {}) {
    const char* samplerMaterial =
        R"({ "passes": [{ "shader": "_s", "blending": "translucent", "textures": [] }] })";
    std::vector<std::pair<std::string, std::string>> files {
        { "/shaders/_s.vert", kTrivialVert },
        { "/shaders/_s.frag", kTwoSamplerFrag },
        { "/shaders/_i.vert", kTrivialVert },
        { "/shaders/_i.frag", kImageTintFrag },
        { "/materials/_s.json", samplerMaterial },
        { "/materials/_i.json",
          // One (empty) texture slot: a puppet composite clears slot 0, so a
          // material with no slots at all would index past the end.
          R"({ "passes": [{ "shader": "_i", "blending": "translucent", "textures": [""] }] })" },
        { "/materials/_add.json",
          R"({ "passes": [{ "shader": "_t", "blending": "additive", "textures": [] }] })" },
        { "/effects/tint.json",
          R"({ "name": "tint", "passes": [{ "material": "materials/_s.json" }] })" },
        { "/materials/util/effectpassthrough.json", samplerMaterial },
        { "/models/util/composelayer.json", kPlainImageJson },
        { "/models/_add.json",
          R"({ "material": "materials/_add.json", "width": 256, "height": 256 })" },
        { "/models/_full.json",
          R"({ "material": "materials/_plain.json", "width": 256, "height": 256,
               "fullscreen": true })" },
    };
    files.insert(files.end(), extras.begin(), extras.end());
    return makeAssetsVfsFrom(files);
}

} // namespace wek::test
