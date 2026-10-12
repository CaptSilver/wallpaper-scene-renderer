#include <doctest.h>

#include "EffectUniformAnimation.hpp"
#include "Scene/SceneMesh.h"

#include <memory>

using namespace wallpaper;

namespace
{
PropertyAnimation fadeOut() {
    PropertyAnimation a;
    a.mode      = PropertyAnimMode::Single;
    a.fps       = 1.0f;
    a.length    = 2.0f;
    a.keyframes = { { 0.0f, 1.0f }, { 2.0f, 0.0f } };
    a.playing   = true;
    return a;
}
} // namespace

TEST_SUITE("TickEffectUniformAnimations") {
    TEST_CASE("a moving curve writes its uniform and marks the material dirty") {
        auto mesh = std::make_shared<SceneMesh>();
        mesh->AddMaterial(SceneMaterial {});
        auto& mat                               = *mesh->Material();
        mat.customShader.constValues["g_Alpha"] = { 1.0f };
        std::vector<Scene::EffectUniformAnimation> anims;
        anims.push_back({ mesh, "g_Alpha", { { 1.0f }, { { 0, fadeOut() } } } });

        TickEffectUniformAnimations(anims, 1.0);
        CHECK(mat.customShader.constValues.at("g_Alpha")[0] == doctest::Approx(0.5f));
        CHECK(mat.customShader.constValuesDirty);
    }

    TEST_CASE("a curve that holds still leaves the material clean") {
        auto mesh = std::make_shared<SceneMesh>();
        mesh->AddMaterial(SceneMaterial {});
        auto& mat                               = *mesh->Material();
        mat.customShader.constValues["g_Alpha"] = { 0.0f };
        std::vector<Scene::EffectUniformAnimation> anims;
        anims.push_back({ mesh, "g_Alpha", { { 0.0f }, { { 0, fadeOut() } } } });
        TickEffectUniformAnimations(anims, 5.0); // jumps to the end: 1 -> 0 is a change
        mat.customShader.constValuesDirty = false;
        TickEffectUniformAnimations(anims, 1.0);
        CHECK_FALSE(mat.customShader.constValuesDirty);
    }

    TEST_CASE("the record keeps a dropped effect's material alive") {
        // An effect discarded after registration (a later pass failed, or its
        // shader did not compile) frees its node; the tick must not write
        // into freed memory.
        std::vector<Scene::EffectUniformAnimation> anims;
        {
            auto mesh = std::make_shared<SceneMesh>();
            mesh->AddMaterial(SceneMaterial {});
            anims.push_back({ mesh, "g_Alpha", { { 1.0f }, { { 0, fadeOut() } } } });
        }
        TickEffectUniformAnimations(anims, 1.0);
        CHECK(anims[0].mesh->Material()->customShader.constValues.at("g_Alpha")[0] ==
              doctest::Approx(0.5f));
    }
}
