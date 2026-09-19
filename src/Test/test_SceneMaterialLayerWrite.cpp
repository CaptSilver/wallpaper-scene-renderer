#include <doctest.h>

#include "SceneMaterialLayerWrite.hpp"
#include "Scene/SceneMaterial.h"

#include <vector>

using namespace wallpaper;

namespace
{
// A material seeded the way WPSceneParser seeds every image layer: the
// image-shader pair plus the flat-shader pair.
SceneMaterial seededMaterial() {
    SceneMaterial m;
    auto&         cv                = m.customShader.constValues;
    cv["g_UserAlpha"]               = std::vector<float> { 1.0f };
    cv["g_Color4"]                  = std::vector<float> { 0.1f, 0.2f, 0.3f, 1.0f };
    cv["g_Alpha"]                   = std::vector<float> { 1.0f };
    cv["g_Color"]                   = std::vector<float> { 0.1f, 0.2f, 0.3f };
    m.customShader.constValuesDirty = false;
    return m;
}
} // namespace

TEST_SUITE("SceneMaterialLayerWrite") {
    TEST_CASE("writeLayerAlpha updates g_UserAlpha and g_Color4.a, not the flat pair") {
        auto m = seededMaterial();
        writeLayerAlpha(m, 0.25f);
        const auto& cv = m.customShader.constValues;
        CHECK(cv.at("g_UserAlpha")[0] == doctest::Approx(0.25f));
        CHECK(cv.at("g_Color4")[3] == doctest::Approx(0.25f));
        CHECK(cv.at("g_Color4")[0] == doctest::Approx(0.1f));
        CHECK(cv.at("g_Alpha")[0] == doctest::Approx(1.0f));
        CHECK(m.customShader.constValuesDirty);
    }

    TEST_CASE("writeFlatAlpha drives g_Alpha for the flat shader") {
        auto m = seededMaterial();
        writeFlatAlpha(m, 0.0f);
        CHECK(m.customShader.constValues.at("g_Alpha")[0] == doctest::Approx(0.0f));
        CHECK(m.customShader.constValues.at("g_UserAlpha")[0] == doctest::Approx(1.0f));
        CHECK(m.customShader.constValuesDirty);
    }

    TEST_CASE("writeFlatAlpha leaves a material without g_Alpha untouched") {
        SceneMaterial m;
        m.customShader.constValues["g_UserAlpha"] = std::vector<float> { 1.0f };
        writeFlatAlpha(m, 0.0f);
        CHECK(m.customShader.constValues.count("g_Alpha") == 0);
        CHECK_FALSE(m.customShader.constValuesDirty);
    }

    TEST_CASE("writeLayerColor rewrites g_Color4.rgb and keeps its alpha") {
        auto m = seededMaterial();
        writeLayerAlpha(m, 0.5f);
        writeLayerColor(m, 0.7f, 0.8f, 0.9f);
        const auto& c4 = m.customShader.constValues.at("g_Color4");
        CHECK(c4[0] == doctest::Approx(0.7f));
        CHECK(c4[1] == doctest::Approx(0.8f));
        CHECK(c4[2] == doctest::Approx(0.9f));
        CHECK(c4[3] == doctest::Approx(0.5f));
        CHECK(m.customShader.constValues.at("g_Color")[0] == doctest::Approx(0.1f));
    }

    TEST_CASE("writeLayerColor on a material without g_Color4 assumes alpha 1") {
        SceneMaterial m;
        writeLayerColor(m, 0.7f, 0.8f, 0.9f);
        const auto& c4 = m.customShader.constValues.at("g_Color4");
        CHECK(c4.size() == 4);
        CHECK(c4[3] == doctest::Approx(1.0f));
    }

    TEST_CASE("writeFlatColor drives g_Color for the flat shader") {
        auto m = seededMaterial();
        writeFlatColor(m, 0.7f, 0.8f, 0.9f);
        const auto& c = m.customShader.constValues.at("g_Color");
        CHECK(c[0] == doctest::Approx(0.7f));
        CHECK(c[1] == doctest::Approx(0.8f));
        CHECK(c[2] == doctest::Approx(0.9f));
        CHECK(m.customShader.constValues.at("g_Color4")[0] == doctest::Approx(0.1f));
        CHECK(m.customShader.constValuesDirty);
    }

    TEST_CASE("writeFlatColor leaves a material without g_Color untouched") {
        SceneMaterial m;
        writeFlatColor(m, 0.7f, 0.8f, 0.9f);
        CHECK(m.customShader.constValues.count("g_Color") == 0);
        CHECK_FALSE(m.customShader.constValuesDirty);
    }
}
