#include <doctest.h>

#include <QCoreApplication>
#include <QJSValue>

#include <memory>
#include <string>
#include <vector>

#include "SceneBackend.hpp"
#include "IPropertyDispatchSink.hpp"

// Drives the real 30 Hz SceneScript property/text/color dispatch in
// SceneBackend.cpp against a recording fake, with no SceneWallpaper / Vulkan
// device.  WEKDE_TEST_NO_SCENE is set in test_main_qt.cpp before any
// SceneObject is constructed, so the ctor leaves m_scene null and the test
// injects its own dispatch sink.
//
// A script that returns a wall-clock-independent constant makes each case
// deterministic: the sink either records exactly the expected call or (on the
// idempotence case) records nothing on the second tick.

namespace
{

// Records every dispatch call as a flat (method, stringified-args) tuple so a
// test can assert on the exact args the eval loop produced.  Args are rendered
// deterministically; floats via the same %g the doctest asserts read back.
struct RecordingDispatchSink : scenebackend::IPropertyDispatchSink {
    struct Call {
        std::string        method;
        int32_t            id { 0 };
        int32_t            i0 { 0 }; // secondary int arg (effectIdx / index)
        std::string        s0;       // primary string arg (property / name / text)
        std::string        s1;       // secondary string arg (valign / fontName)
        std::string        s2;       // tertiary string arg (fontName)
        bool               b0 { false };
        std::vector<float> floats;
    };
    std::vector<Call> calls;

    void updateText(int32_t id, const std::string& text) override {
        calls.push_back({ "updateText", id, 0, text, {}, {}, false, {} });
    }
    void updateTextPointsize(int32_t id, float pointsize) override {
        calls.push_back({ "updateTextPointsize", id, 0, {}, {}, {}, false, { pointsize } });
    }
    void updateColor(int32_t id, float r, float g, float b) override {
        calls.push_back({ "updateColor", id, 0, {}, {}, {}, false, { r, g, b } });
    }
    void updateNodeTransform(int32_t id, const std::string& property, float x, float y,
                             float z) override {
        calls.push_back({ "updateNodeTransform", id, 0, property, {}, {}, false, { x, y, z } });
    }
    void updateNodeVisible(int32_t id, bool visible) override {
        calls.push_back({ "updateNodeVisible", id, 0, {}, {}, {}, visible, {} });
    }
    void updateNodeAlpha(int32_t id, float alpha) override {
        calls.push_back({ "updateNodeAlpha", id, 0, {}, {}, {}, false, { alpha } });
    }
    void updateParticleRate(int32_t id, float rate) override {
        calls.push_back({ "updateParticleRate", id, 0, {}, {}, {}, false, { rate } });
    }
    void applyLayerBatch(
        const std::vector<wallpaper::SceneWallpaper::LayerBatchUpdate>& batch) override {
        for (const auto& u : batch) {
            Call c { "applyLayerBatch", u.id, (int32_t)u.flags, {}, {}, {}, u.visible != 0, {} };
            c.floats = { u.origin[0], u.origin[1], u.origin[2], u.scale[0],  u.scale[1],
                         u.scale[2],  u.angles[0], u.angles[1], u.angles[2], u.alpha };
            calls.push_back(std::move(c));
        }
    }
    void updateEffectVisible(int32_t nodeId, int32_t effectIndex, bool visible) override {
        calls.push_back({ "updateEffectVisible", nodeId, effectIndex, {}, {}, {}, visible, {} });
    }
    void updateMaterialValue(int32_t nodeId, std::string name, std::vector<float> floats) override {
        calls.push_back({ "updateMaterialValue",
                          nodeId,
                          0,
                          std::move(name),
                          {},
                          {},
                          false,
                          std::move(floats) });
    }
    void updateEffectMaterialValue(int32_t nodeId, int32_t effectIdx, std::string name,
                                   std::vector<float> floats) override {
        calls.push_back({ "updateEffectMaterialValue",
                          nodeId,
                          effectIdx,
                          std::move(name),
                          {},
                          {},
                          false,
                          std::move(floats) });
    }
    void setLayerSpriteFrame(int32_t nodeId, bool wantsManual, int32_t frameIdx) override {
        calls.push_back({ "setLayerSpriteFrame", nodeId, frameIdx, {}, {}, {}, wantsManual, {} });
    }
    void updateTextStyle(int32_t nodeId, std::string halign, std::string valign,
                         std::string fontName) override {
        calls.push_back({ "updateTextStyle",
                          nodeId,
                          0,
                          std::move(halign),
                          std::move(valign),
                          std::move(fontName),
                          false,
                          {} });
    }
    void updateSoundVolume(int32_t index, float volume) override {
        calls.push_back({ "updateSoundVolume", index, 0, {}, {}, {}, false, { volume } });
    }
    void updateClearColor(float r, float g, float b) override {
        calls.push_back({ "updateClearColor", 0, 0, {}, {}, {}, false, { r, g, b } });
    }
    void updateBloomStrength(float strength) override {
        calls.push_back({ "updateBloomStrength", 0, 0, {}, {}, {}, false, { strength } });
    }
    void updateBloomThreshold(float threshold) override {
        calls.push_back({ "updateBloomThreshold", 0, 0, {}, {}, {}, false, { threshold } });
    }
    void updateCameraFov(float fov) override {
        calls.push_back({ "updateCameraFov", 0, 0, {}, {}, {}, false, { fov } });
    }
    void updateCameraLookAt(float ex, float ey, float ez, float cx, float cy, float cz, float ux,
                            float uy, float uz) override {
        calls.push_back({ "updateCameraLookAt",
                          0,
                          0,
                          {},
                          {},
                          {},
                          false,
                          { ex, ey, ez, cx, cy, cz, ux, uy, uz } });
    }
    void updateAmbientColor(float r, float g, float b) override {
        calls.push_back({ "updateAmbientColor", 0, 0, {}, {}, {}, false, { r, g, b } });
    }
    void updateSkylightColor(float r, float g, float b) override {
        calls.push_back({ "updateSkylightColor", 0, 0, {}, {}, {}, false, { r, g, b } });
    }
    void updateLightColor(int32_t index, float r, float g, float b) override {
        calls.push_back({ "updateLightColor", index, 0, {}, {}, {}, false, { r, g, b } });
    }
    void updateLightRadius(int32_t index, float radius) override {
        calls.push_back({ "updateLightRadius", index, 0, {}, {}, {}, false, { radius } });
    }
    void updateLightIntensity(int32_t index, float intensity) override {
        calls.push_back({ "updateLightIntensity", index, 0, {}, {}, {}, false, { intensity } });
    }
    void updateLightPosition(int32_t index, float x, float y, float z) override {
        calls.push_back({ "updateLightPosition", index, 0, {}, {}, {}, false, { x, y, z } });
    }

    const Call* find(const std::string& method) const {
        for (const auto& c : calls)
            if (c.method == method) return &c;
        return nullptr;
    }
    int count(const std::string& method) const {
        int n = 0;
        for (const auto& c : calls)
            if (c.method == method) ++n;
        return n;
    }
};

// Build a SceneObject with the recording sink installed.  m_scene stays null
// (WEKDE_TEST_NO_SCENE), so this exercises the null-m_scene eval path.
std::unique_ptr<scenebackend::SceneObject> makeObj(RecordingDispatchSink** outSink) {
    auto obj  = std::make_unique<scenebackend::SceneObject>();
    auto sink = std::make_unique<RecordingDispatchSink>();
    *outSink  = sink.get();
    obj->setDispatchSinkForTesting(std::move(sink));
    return obj;
}

using Kind = scenebackend::SceneObject::TestScriptKind;

} // namespace

TEST_SUITE("SceneScript dispatch sink") {
    TEST_CASE("visible script returns false -> updateNodeVisible(id,false) once") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);
        // property scripts seed currentVisible=true; returning false is a change.
        obj->seedPropertyScriptForTesting(
            5, Kind::Visible, "visible", "(function(v){ return false; })");
        obj->evaluatePropertyScriptsForTesting();

        REQUIRE(sink->count("updateNodeVisible") == 1);
        const auto* c = sink->find("updateNodeVisible");
        REQUIRE(c != nullptr);
        CHECK(c->id == 5);
        CHECK(c->b0 == false);
    }

    TEST_CASE("vec3 origin script returns Vec3(10,20,30) -> updateNodeTransform") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);
        obj->seedPropertyScriptForTesting(
            5, Kind::Vec3, "origin", "(function(v){ return new Vec3(10,20,30); })");
        obj->evaluatePropertyScriptsForTesting();

        REQUIRE(sink->count("updateNodeTransform") == 1);
        const auto* c = sink->find("updateNodeTransform");
        REQUIRE(c != nullptr);
        CHECK(c->id == 5);
        CHECK(c->s0 == "origin");
        REQUIRE(c->floats.size() == 3);
        CHECK(c->floats[0] == doctest::Approx(10.0f));
        CHECK(c->floats[1] == doctest::Approx(20.0f));
        CHECK(c->floats[2] == doctest::Approx(30.0f));
    }

    TEST_CASE("text script sets thisLayer.font -> updateTextStyle") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);
        // updateTextStyle dispatches from the setTextStyle bridge (the JS shim
        // for thisLayer.font routes through it), so seeding drives the sink.
        obj->seedTextStyleScriptForTesting(5, "", "", "Heavy.otf");

        REQUIRE(sink->count("updateTextStyle") == 1);
        const auto* c = sink->find("updateTextStyle");
        REQUIRE(c != nullptr);
        CHECK(c->id == 5);
        CHECK(c->s0 == "");          // halign
        CHECK(c->s1 == "");          // valign
        CHECK(c->s2 == "Heavy.otf"); // fontName
    }

    TEST_CASE("multi-mutation tick: two scripts, two layers, order preserved") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);
        obj->seedPropertyScriptForTesting(
            7, Kind::Visible, "visible", "(function(v){ return false; })");
        obj->seedPropertyScriptForTesting(9, Kind::Alpha, "alpha", "(function(v){ return 0.25; })");
        obj->evaluatePropertyScriptsForTesting();

        REQUIRE(sink->calls.size() == 2);
        // stable_sort puts Visible (kind 0) before Alpha (kind 2), so the
        // visible dispatch is recorded first.
        CHECK(sink->calls[0].method == "updateNodeVisible");
        CHECK(sink->calls[0].id == 7);
        CHECK(sink->calls[1].method == "updateNodeAlpha");
        CHECK(sink->calls[1].id == 9);
        REQUIRE(sink->calls[1].floats.size() == 1);
        CHECK(sink->calls[1].floats[0] == doctest::Approx(0.25f));
    }

    TEST_CASE("idempotence: same value next tick -> sink not called again") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);
        obj->seedPropertyScriptForTesting(5, Kind::Alpha, "alpha", "(function(v){ return 0.5; })");
        obj->evaluatePropertyScriptsForTesting();
        REQUIRE(sink->count("updateNodeAlpha") == 1);

        // Second tick returns the same value; the JS delta-threshold in
        // _runAllPropertyScripts suppresses it, so no new dispatch.
        obj->evaluatePropertyScriptsForTesting();
        CHECK(sink->count("updateNodeAlpha") == 1);
    }
}
