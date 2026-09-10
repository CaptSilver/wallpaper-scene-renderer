#include <doctest.h>

#include <QJSValue>
#include <QString>

#include <memory>
#include <string>
#include <vector>

#include "SceneBackend.hpp"
#include "IPropertyDispatchSink.hpp"

// The 30 Hz property-script loop only forwards a value to the renderer when it
// differs from the one it last forwarded.  These cases pin where that line
// sits: scripts that live at very small magnitudes (lens-flare scales in the
// 1e-5 range, alpha fades converging on 0) have to keep moving, while a script
// returning the value it returned last tick still costs nothing.

namespace
{

// Records the three node mutations the property loop can produce; every other
// sink method is an ignored no-op because no case here drives one.
struct TransformSink : scenebackend::IPropertyDispatchSink {
    struct Call {
        std::string method;
        int32_t     id { 0 };
        std::string property;
        float       x { 0 }, y { 0 }, z { 0 };
        bool        visible { false };
    };
    std::vector<Call> calls;

    void updateNodeTransform(int32_t id, const std::string& property, float x, float y,
                             float z) override {
        calls.push_back({ "updateNodeTransform", id, property, x, y, z, false });
    }
    // Single-float mutations land in the x slot.
    void updateNodeAlpha(int32_t id, float alpha) override {
        calls.push_back({ "updateNodeAlpha", id, {}, alpha, 0, 0, false });
    }
    void updateNodeVisible(int32_t id, bool visible) override {
        calls.push_back({ "updateNodeVisible", id, {}, 0, 0, 0, visible });
    }

    void updateText(int32_t, const std::string&) override {}
    void updateTextPointsize(int32_t, float) override {}
    void updateColor(int32_t, float, float, float) override {}
    void updateParticleRate(int32_t, float) override {}
    void applyLayerBatch(const std::vector<wallpaper::SceneWallpaper::LayerBatchUpdate>&) override {
    }
    void updateEffectVisible(int32_t, int32_t, bool) override {}
    void updateMaterialValue(int32_t, std::string, std::vector<float>) override {}
    void updateEffectMaterialValue(int32_t, int32_t, std::string, std::vector<float>) override {}
    void setLayerSpriteFrame(int32_t, bool, int32_t) override {}
    void updateTextStyle(int32_t, std::string, std::string, std::string) override {}
    void updateSoundVolume(int32_t, float) override {}
    void updateClearColor(float, float, float) override {}
    void updateBloomStrength(float) override {}
    void updateBloomThreshold(float) override {}
    void updateCameraFov(float) override {}
    void updateCameraLookAt(float, float, float, float, float, float, float, float,
                            float) override {}
    void updateAmbientColor(float, float, float) override {}
    void updateSkylightColor(float, float, float) override {}
    void updateLightColor(int32_t, float, float, float) override {}
    void updateLightRadius(int32_t, float) override {}
    void updateLightIntensity(int32_t, float) override {}
    void updateLightPosition(int32_t, float, float, float) override {}

    int count(const std::string& method) const {
        int n = 0;
        for (const auto& c : calls)
            if (c.method == method) ++n;
        return n;
    }
    const Call* last(const std::string& method) const {
        const Call* found = nullptr;
        for (const auto& c : calls)
            if (c.method == method) found = &c;
        return found;
    }
};

std::unique_ptr<scenebackend::SceneObject> makeObj(TransformSink** outSink) {
    auto obj  = std::make_unique<scenebackend::SceneObject>();
    auto sink = std::make_unique<TransformSink>();
    *outSink  = sink.get();
    obj->setDispatchSinkForTesting(std::move(sink));
    return obj;
}

using Kind = scenebackend::SceneObject::TestScriptKind;

} // namespace

TEST_SUITE("Property script change gate") {
    TEST_CASE("a scale script working in the 1e-5 range reaches the renderer") {
        TransformSink* sink = nullptr;
        auto           obj  = makeObj(&sink);
        obj->seedPropertyScriptForTesting(
            5, Kind::Vec3, "scale", "(function(v){ return new Vec3(2e-5, 2e-5, 2e-5); })");
        obj->evaluatePropertyScriptsForTesting();

        REQUIRE(sink->count("updateNodeTransform") == 1);
        const auto* c = sink->last("updateNodeTransform");
        REQUIRE(c != nullptr);
        CHECK(c->x == doctest::Approx(2e-5f));
    }

    TEST_CASE("a shrinking scale keeps shrinking past 1e-4") {
        TransformSink* sink = nullptr;
        auto           obj  = makeObj(&sink);
        // Halves the value it was handed, so it only keeps moving if each
        // dispatched value is fed back in as the next tick's input.
        obj->seedPropertyScriptForTesting(5,
                                          Kind::Vec3,
                                          "scale",
                                          "(function(){ var first = true;\n"
                                          "  return function(v) {\n"
                                          "    if (first) { first = false;\n"
                                          "      return new Vec3(1e-3, 1e-3, 1e-3); }\n"
                                          "    return new Vec3(v.x*0.5, v.y*0.5, v.z*0.5);\n"
                                          "  }; })()");
        for (int i = 0; i < 12; ++i) obj->evaluatePropertyScriptsForTesting();

        const auto* c = sink->last("updateNodeTransform");
        REQUIRE(c != nullptr);
        CHECK(c->x < 1e-5f);
    }

    TEST_CASE("an alpha fade converges on zero instead of leaving a residue") {
        TransformSink* sink = nullptr;
        auto           obj  = makeObj(&sink);
        // Alpha seeds at 1.0, so halving each tick is a plain exponential fade.
        obj->seedPropertyScriptForTesting(
            5, Kind::Alpha, "alpha", "(function(v){ return v * 0.5; })");
        for (int i = 0; i < 20; ++i) obj->evaluatePropertyScriptsForTesting();

        const auto* c = sink->last("updateNodeAlpha");
        REQUIRE(c != nullptr);
        CHECK(c->x < 1e-5f);
    }

    TEST_CASE("a script returning last tick's value dispatches once") {
        TransformSink* sink = nullptr;
        auto           obj  = makeObj(&sink);
        obj->seedPropertyScriptForTesting(
            5, Kind::Vec3, "origin", "(function(v){ return new Vec3(3, 4, 5); })");
        for (int i = 0; i < 5; ++i) obj->evaluatePropertyScriptsForTesting();

        CHECK(sink->count("updateNodeTransform") == 1);
    }

    TEST_CASE("a large position moving in y and z is not judged by its x component") {
        TransformSink* sink = nullptr;
        auto           obj  = makeObj(&sink);
        obj->seedPropertyScriptForTesting(5,
                                          Kind::Vec3,
                                          "origin",
                                          "(function(){ var n = 0;\n"
                                          "  return function(v) { n += 1;\n"
                                          "    return new Vec3(-2198, 195 + n*0.5, 846 - n*0.5);\n"
                                          "  }; })()");
        for (int i = 0; i < 3; ++i) obj->evaluatePropertyScriptsForTesting();

        CHECK(sink->count("updateNodeTransform") == 3);
        const auto* c = sink->last("updateNodeTransform");
        REQUIRE(c != nullptr);
        CHECK(c->y == doctest::Approx(196.5f));
        CHECK(c->z == doctest::Approx(844.5f));
    }

    TEST_CASE("jitter below what a float can hold is still suppressed") {
        TransformSink* sink = nullptr;
        auto           obj  = makeObj(&sink);
        // 1e-9 on a value near 2200 lands well inside one float32 step, so the
        // renderer would receive an identical float every tick.
        obj->seedPropertyScriptForTesting(5,
                                          Kind::Vec3,
                                          "origin",
                                          "(function(){ var n = 0;\n"
                                          "  return function(v) { n += 1;\n"
                                          "    return new Vec3(-2198 + n*1e-9, 195, 846);\n"
                                          "  }; })()");
        for (int i = 0; i < 5; ++i) obj->evaluatePropertyScriptsForTesting();

        CHECK(sink->count("updateNodeTransform") == 1);
    }
}
