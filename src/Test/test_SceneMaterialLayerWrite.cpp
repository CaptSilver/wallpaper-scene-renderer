#include <doctest.h>

#include "SceneMaterialLayerWrite.hpp"
#include "Scene/SceneImageEffectLayer.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneNode.h"

#include <memory>
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

std::shared_ptr<SceneNode> effectNodeWithMaterial(SceneMaterial m) {
    auto node = std::make_shared<SceneNode>();
    auto mesh = std::make_shared<SceneMesh>();
    mesh->AddMaterial(std::move(m));
    node->AddMesh(mesh);
    return node;
}
} // namespace

TEST_SUITE("SceneMaterialLayerWrite") {
    TEST_CASE("writeAlphaToEffectLayer skips the synthesized passthrough, whose colour is "
              "neutral because the base pass already baked the alpha in") {
        SceneNode             world;
        SceneImageEffectLayer layer(&world, "pp_a", "pp_b");

        auto real = std::make_shared<SceneImageEffect>();
        real->nodes.push_back({ "pp_b", effectNodeWithMaterial(seededMaterial()) });
        layer.AddEffect(real);

        auto synth                                    = std::make_shared<SceneImageEffect>();
        synth->synthesized                            = true;
        auto synthMat                                 = seededMaterial();
        synthMat.customShader.constValues["g_Color4"] = std::vector<float> { 1, 1, 1, 1 };
        synth->nodes.push_back({ "pp_b", effectNodeWithMaterial(std::move(synthMat)) });
        layer.AddEffect(synth);

        writeAlphaToEffectLayer(layer, 0.25f);

        const auto& realCv =
            real->nodes.front().sceneNode->Mesh()->Material()->customShader.constValues;
        CHECK(realCv.at("g_UserAlpha")[0] == doctest::Approx(0.25f));
        CHECK(realCv.at("g_Color4")[3] == doctest::Approx(0.25f));

        const auto& synthCv =
            synth->nodes.front().sceneNode->Mesh()->Material()->customShader.constValues;
        CHECK(synthCv.at("g_Color4")[3] == doctest::Approx(1.0f));
        CHECK(synthCv.at("g_UserAlpha")[0] == doctest::Approx(1.0f));
    }

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

TEST_SUITE("SceneMaterial: ResolveShaderConstantName") {
    TEST_CASE("a name the shader aliases resolves to its declared GLSL uniform") {
        SceneMaterial m;
        m.customShader.alias["bloomstrength"] = "g_BloomStrength";
        CHECK(ResolveShaderConstantName(m.customShader, "bloomstrength") == "g_BloomStrength");
    }

    TEST_CASE("a name with no alias entry passes through unchanged") {
        SceneMaterial m;
        m.customShader.alias["bloomstrength"] = "g_BloomStrength"; // unrelated entry present
        CHECK(ResolveShaderConstantName(m.customShader, "bloomthreshold") == "bloomthreshold");
    }
}
