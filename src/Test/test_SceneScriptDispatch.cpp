#include <doctest.h>

#include <QCoreApplication>
#include <QJSValue>
#include <QString>

#include <QMetaMethod>
#include <QMetaObject>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "SceneBackend.hpp"
#include "SceneScriptBridge.h"
#include "IPropertyDispatchSink.hpp"
#include "Utils/Logging.h"

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

// The `__sceneBridge` global is the one C++ object untrusted Steam-Workshop
// SceneScript can reach.  Scene bodies are lifted verbatim out of scene.json
// (WPPropertyScriptExtract.hpp) and compiled into this same QJSEngine, so
// whatever `__sceneBridge` reflects is granted to hostile content.  These cases
// pin that surface to the shim allowlist from both directions: the dangerous
// names must be absent, and all 22 names the shims dispatch through must stay
// callable.
TEST_SUITE("SceneScript bridge surface") {
    // Bootstraps the JS engine through the only public seam that does it, then
    // hands back an object whose `__sceneBridge` global is installed exactly as
    // production installs it.
    std::unique_ptr<scenebackend::SceneObject> makeBridgedObj(RecordingDispatchSink * *outSink) {
        auto obj = makeObj(outSink);
        obj->seedPropertyScriptForTesting(1, Kind::Alpha, "alpha", "(function(v){ return v; })");
        return obj;
    }

    TEST_CASE("scene script cannot reach the debug/tooling methods through __sceneBridge") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeBridgedObj(&sink);

        // Guards against a vacuous pass: every assertion below would also hold
        // if no bridge were installed at all.
        REQUIRE(obj->debugEvalJs("typeof __sceneBridge").toStdString() == "object");

        for (const char* m : { "requestScreenshot",
                               "requestPassDump",
                               "setHidePattern",
                               "debugEvalJs",
                               "simulateClickAt",
                               "simulateHoverAt",
                               "simulateDragAt" }) {
            INFO("member: " << std::string(m));
            CHECK(
                obj->debugEvalJs(QStringLiteral("typeof __sceneBridge.%1").arg(m)).toStdString() ==
                "undefined");
        }
    }

    TEST_CASE("a scene.json property script body cannot call into arbitrary C++") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);
        // The string below is the same shape WPPropertyScriptExtract lifts out
        // of an untrusted scene.json, dispatched through the real tick loop.
        obj->seedPropertyScriptForTesting(
            5,
            Kind::Alpha,
            "alpha",
            "(function(v){ try { __sceneBridge.requestScreenshot('/tmp/wek-escape-probe.ppm');"
            "                    _probe.v = 'reached'; }"
            "              catch(e) { _probe.v = 'blocked'; } return v; })");
        // Container object so the script body writes a property rather than
        // creating an implicit global.
        obj->debugEvalJs("var _probe = { v: '' };");
        obj->evaluatePropertyScriptsForTesting();

        // No file-existence assertion on purpose: under WEKDE_TEST_NO_SCENE
        // m_scene is null, so SceneObject::requestScreenshot no-ops and nothing
        // is ever written here.  The reachability of the call is the defect; the
        // write itself lands in VulkanRender's fopen(path, "wb").
        CHECK(obj->debugEvalJs("_probe.v").toStdString() == "blocked");
    }

    TEST_CASE("scene script cannot reach SceneObject or QQuickItem properties") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeBridgedObj(&sink);

        // Declared on SceneObject.  Assignment is deliberately never attempted:
        // `__sceneBridge.source = ...` really runs setSource, which deletes the
        // running QJSEngine out from under the caller.
        for (const char* p : { "source",
                               "assets",
                               "userProperties",
                               "postprocessingOverride",
                               "renderPixelWidth",
                               "renderPixelHeight",
                               "fps" }) {
            INFO("member: " << std::string(p));
            CHECK(
                obj->debugEvalJs(QStringLiteral("typeof __sceneBridge.%1").arg(p)).toStdString() ==
                "undefined");
        }

        // Inherited from QQuickItem — the larger half of the surface, and not
        // this project's code.  `grabToImage` hands its callback a
        // QQuickItemGrabResult carrying a Q_INVOKABLE saveToFile(), i.e. a
        // second arbitrary-file-write path; `parent` walks the live item tree.
        for (const char* p : { "parent", "grabToImage", "mapToGlobal", "visible", "enabled" }) {
            INFO("member: " << std::string(p));
            CHECK(
                obj->debugEvalJs(QStringLiteral("typeof __sceneBridge.%1").arg(p)).toStdString() ==
                "undefined");
        }

        // Whole-surface bound rather than a name-by-name list: wrapping the
        // QQuickItem reflects 88 enumerable names, the narrow bridge 23.
        CAPTURE(obj->debugEvalJs("Object.keys(__sceneBridge).length").toStdString());
        CHECK(obj->debugEvalJs("Object.keys(__sceneBridge).length < 30").toStdString() == "true");
    }

    TEST_CASE("the shim surface scripts depend on stays intact") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeBridgedObj(&sink);

        // Every name the first-party JS shims dispatch through.  The shims guard
        // each call with `typeof __sceneBridge.X === 'function'`, so a dropped or
        // misspelled forwarder silently stops working instead of erroring —
        // enumerate all of them rather than spot-checking.
        for (const char* m : { "materialSetValue",
                               "effectMaterialSetValue",
                               "setLayerSpriteFrame",
                               "getLayerSpriteInfo",
                               "setTextStyle",
                               "getLayerWorldTransform",
                               "getBoneIndex",
                               "setLayerParent",
                               "sortLayer",
                               "openUserShortcut",
                               "lsGet",
                               "lsSet",
                               "lsRemove",
                               "lsClear",
                               "videoGetCurrentTime",
                               "videoGetDuration",
                               "videoIsPlaying",
                               "videoPlay",
                               "videoPause",
                               "videoStop",
                               "videoSetCurrentTime",
                               "videoSetRate" }) {
            INFO("member: " << std::string(m));
            CHECK(
                obj->debugEvalJs(QStringLiteral("typeof __sceneBridge.%1").arg(m)).toStdString() ==
                "function");
        }
    }

    TEST_CASE("a shim call through __sceneBridge still reaches the dispatch sink") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeBridgedObj(&sink);
        // Registers "testlayer_5" -> 5 in the layer-name map (and records one
        // updateTextStyle call, which is why the assertion below is specific).
        obj->seedTextStyleScriptForTesting(5, "", "", "seed.otf");

        obj->debugEvalJs("__sceneBridge.materialSetValue('testlayer_5','g_Color',[1,0,0]);");

        REQUIRE(sink->count("updateMaterialValue") == 1);
        const auto* c = sink->find("updateMaterialValue");
        REQUIRE(c != nullptr);
        CHECK(c->id == 5);
        CHECK(c->s0 == "g_Color");
    }

    TEST_CASE("a scene script cannot hijack the __sceneBridge global") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeBridgedObj(&sink);
        obj->seedTextStyleScriptForTesting(5, "", "", "seed.otf");

        // Every first-party shim resolves `__sceneBridge` by global name at call
        // time, so a script that lands its own object there intercepts all of
        // them.  Neither assignment nor redefinition may take.
        obj->debugEvalJs(
            "try { __sceneBridge = { materialSetValue: function(){} }; } catch (e) {}");
        obj->debugEvalJs("try { Object.defineProperty(this, '__sceneBridge', "
                         "{ value: { materialSetValue: function(){} } }); } catch (e) {}");
        obj->debugEvalJs("try { delete __sceneBridge; } catch (e) {}");

        // Still the real bridge: the call reaches C++, not the impostor.
        obj->debugEvalJs("__sceneBridge.materialSetValue('testlayer_5','g_Color',[1,0,0]);");
        CHECK(sink->count("updateMaterialValue") == 1);
    }

    TEST_CASE("the destroy-time bridge re-install keeps shim calls working") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeBridgedObj(&sink);
        obj->seedTextStyleScriptForTesting(5, "", "", "seed.otf");

        // fireDestroyEvent re-installs the bridge before running destroy
        // handlers, because a wrapper handed out earlier can go stale by
        // teardown.  If the re-install stops resolving, destroy handlers lose
        // their lsSet calls and user state silently stops persisting.
        obj->reinstallSceneBridgeForTesting();
        obj->reinstallSceneBridgeForTesting();

        CHECK(obj->debugEvalJs("typeof __sceneBridge.lsSet").toStdString() == "function");
        obj->debugEvalJs("__sceneBridge.materialSetValue('testlayer_5','g_Color',[1,0,0]);");
        CHECK(sink->count("updateMaterialValue") == 1);
    }

    TEST_CASE("the bridge metaobject declares nothing beyond the allowlist") {
        // Pins the surface from the C++ side, with no engine involved: the
        // metaobject IS the allowlist, so a Q_INVOKABLE added to the bridge
        // fails here until someone widens this set deliberately.  A null owner
        // is fine — every forwarder guards on it.
        scenebackend::SceneScriptBridge b(nullptr);
        const QMetaObject*              mo = b.metaObject();

        // No Q_PROPERTY of its own, so no script assignment can reach a C++
        // setter (`__sceneBridge.source = ...` used to run SceneObject::setSource,
        // which deletes the QJSEngine that is mid-assignment).
        CHECK(mo->propertyCount() == mo->propertyOffset());

        const std::set<std::string> allowed { "materialSetValue",
                                              "effectMaterialSetValue",
                                              "setLayerSpriteFrame",
                                              "getLayerSpriteInfo",
                                              "setTextStyle",
                                              "getLayerWorldTransform",
                                              "getBoneIndex",
                                              "setLayerParent",
                                              "sortLayer",
                                              "openUserShortcut",
                                              "lsGet",
                                              "lsSet",
                                              "lsRemove",
                                              "lsClear",
                                              "videoGetCurrentTime",
                                              "videoGetDuration",
                                              "videoIsPlaying",
                                              "videoPlay",
                                              "videoPause",
                                              "videoStop",
                                              "videoSetCurrentTime",
                                              "videoSetRate" };
        int                         invokables = 0;
        for (int i = mo->methodOffset(); i < mo->methodCount(); ++i) {
            const QMetaMethod m = mo->method(i);
            if (m.methodType() != QMetaMethod::Method) continue;
            ++invokables;
            INFO("method: " << m.name().toStdString());
            CHECK(allowed.count(m.name().toStdString()) == 1);
        }
        CHECK(invokables == static_cast<int>(allowed.size()));
    }
}

// The media dispatchers and the debounced cursorLeave are author-JS entry
// points with no JS-side try/catch — the QJSValue the call returns is the only
// place a throwing handler can surface.  A media handler that throws on every
// MPRIS track change (or a leave handler that throws and so never fades the
// hover UI back out) has to leave a trail in the journal, otherwise it is
// undiagnosable from a log dump.
namespace
{

// Captures WallpaperLog output so a case can assert a throw was reported.
struct DispatchLogCapture {
    static inline std::string text;
    DispatchLogCapture() {
        text = {};
        wallpaper_log_test::setSink([](int, const char* msg) {
            text += msg;
            text += '\n';
        });
    }
    ~DispatchLogCapture() { wallpaper_log_test::setSink(nullptr); }
    static bool mentions(const char* needle) { return text.find(needle) != std::string::npos; }
};

constexpr const char* kThrower = "(function(ev){ throw new Error('boom'); })";

} // namespace

TEST_SUITE("SceneScript event dispatch error surface") {
    TEST_CASE("a throwing media handler is reported with its id and property") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);

        SUBCASE("mediaPlaybackChanged") {
            obj->seedMediaHandlerForTesting(11, "alpha", "mediaPlaybackChanged", kThrower);
            DispatchLogCapture log;
            obj->mediaPlaybackChanged(1);
            INFO("log: " << DispatchLogCapture::text);
            CHECK(DispatchLogCapture::mentions("mediaPlaybackChanged error id=11 prop=alpha"));
            CHECK(DispatchLogCapture::mentions("boom"));
        }
        SUBCASE("mediaPropertiesChanged") {
            obj->seedMediaHandlerForTesting(12, "origin", "mediaPropertiesChanged", kThrower);
            DispatchLogCapture log;
            obj->mediaPropertiesChanged("t", "a", "al", "aa", "", 60.0);
            INFO("log: " << DispatchLogCapture::text);
            CHECK(DispatchLogCapture::mentions("mediaPropertiesChanged error id=12 prop=origin"));
        }
        SUBCASE("mediaThumbnailChanged") {
            obj->seedMediaHandlerForTesting(13, "color", "mediaThumbnailChanged", kThrower);
            DispatchLogCapture log;
            obj->mediaThumbnailChanged(true, {});
            INFO("log: " << DispatchLogCapture::text);
            CHECK(DispatchLogCapture::mentions("mediaThumbnailChanged error id=13 prop=color"));
        }
        SUBCASE("mediaTimelineChanged") {
            obj->seedMediaHandlerForTesting(14, "scale", "mediaTimelineChanged", kThrower);
            DispatchLogCapture log;
            obj->mediaTimelineChanged(1.0, 60.0, 1);
            INFO("log: " << DispatchLogCapture::text);
            CHECK(DispatchLogCapture::mentions("mediaTimelineChanged error id=14 prop=scale"));
        }
        SUBCASE("mediaStatusChanged") {
            obj->seedMediaHandlerForTesting(15, "visible", "mediaStatusChanged", kThrower);
            DispatchLogCapture log;
            obj->mediaStatusChanged(true);
            INFO("log: " << DispatchLogCapture::text);
            CHECK(DispatchLogCapture::mentions("mediaStatusChanged error id=15 prop=visible"));
        }
    }

    TEST_CASE("a media handler that returns normally logs nothing") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);
        obj->seedMediaHandlerForTesting(
            21, "alpha", "mediaTimelineChanged", "(function(ev){ return ev.position; })");
        DispatchLogCapture log;
        obj->mediaTimelineChanged(3.0, 60.0, 1);
        CHECK_FALSE(DispatchLogCapture::mentions("error"));
    }

    TEST_CASE("every media dispatcher binds thisObject even for an unnamed state") {
        // kPropWrap shadows thisLayer inside the script IIFE but not thisObject,
        // so a state with no layer name must still overwrite the global — else
        // it inherits whatever layer was dispatched last.
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);
        // Seed first — that is what builds the JS engine, and debugEvalJs
        // only queues its snippet until one exists.
        obj->seedMediaHandlerForTesting(
            31, "alpha", "mediaStatusChanged", "(function(ev){ _seen = String(thisObject); })");
        obj->debugEvalJs("var _seen = 'unset'; var thisObject = 'stale';");
        REQUIRE(obj->debugEvalJs("_seen").toStdString() == "unset");
        obj->mediaStatusChanged(true);
        CHECK(obj->debugEvalJs("_seen").toStdString() != "stale");
        CHECK(obj->debugEvalJs("_seen").toStdString() != "unset");
    }

    TEST_CASE("a throwing cursorLeave handler is reported and the log does not claim success") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);
        obj->seedExpiredCursorLeaveForTesting("hoverzone", kThrower);
        DispatchLogCapture log;
        obj->flushPendingCursorLeavesForTesting();
        INFO("log: " << DispatchLogCapture::text);
        CHECK(DispatchLogCapture::mentions("cursorLeave"));
        CHECK(DispatchLogCapture::mentions("hoverzone"));
        CHECK(DispatchLogCapture::mentions("boom"));
        // The pre-call line used to read "cursorLeave: layer 'X' (after grace)"
        // whether or not the handler threw.  Nothing may assert a clean
        // dispatch when the handler blew up.
        CHECK_FALSE(DispatchLogCapture::text.find("(after grace)\n") != std::string::npos);
    }

    TEST_CASE("a cursorLeave handler that returns normally still logs the dispatch") {
        RecordingDispatchSink* sink = nullptr;
        auto                   obj  = makeObj(&sink);
        obj->seedExpiredCursorLeaveForTesting("hoverzone", "(function(ev){ return 1; })");
        DispatchLogCapture log;
        obj->flushPendingCursorLeavesForTesting();
        INFO("log: " << DispatchLogCapture::text);
        CHECK(DispatchLogCapture::mentions("cursorLeave"));
        CHECK(DispatchLogCapture::mentions("hoverzone"));
        CHECK_FALSE(DispatchLogCapture::mentions("boom"));
    }
}
