#include <doctest.h>

#include "EffectUniformAnimation.hpp"
#include "UserPropUniformBinding.hpp"

#include "Scene/Scene.h"
#include "Scene/SceneMaterial.h"
#include "Scene/SceneMesh.h"

#include <memory>

using namespace wallpaper;

namespace
{

WPShaderInfo shaderWithAlias(std::initializer_list<std::pair<const std::string, std::string>> a) {
    WPShaderInfo info;
    for (const auto& [k, v] : a) info.alias[k] = v;
    return info;
}

std::shared_ptr<SceneMesh> meshWithMaterial() {
    auto mesh = std::make_shared<SceneMesh>();
    mesh->AddMaterial(SceneMaterial {});
    return mesh;
}

// What the main thread and the render thread do between them for one update.
void applyAndDrain(const Scene& scene, const nlohmann::json& props) {
    PendingUpdateQueues q;
    wek::applyUserPropUniforms(scene, props, q);
    q.withPropertyLocked([](PendingUpdateQueues& p) {
        wek::drainUserPropUniforms(p.m_pending_user_prop_uniforms);
    });
}

} // namespace

TEST_SUITE("UserPropUniformBinding") {
    TEST_CASE("a direct alias hit wins") {
        auto info = shaderWithAlias({ { "alpha", "g_Alpha" } });
        CHECK(wek::ConstantUniformName(info, "alpha") == "g_Alpha");
    }

    TEST_CASE("a uniform whose name minus g_ matches is found by scan") {
        auto info = shaderWithAlias({ { "x", "g_Alpha" } });
        CHECK(wek::ConstantUniformName(info, "Alpha") == "g_Alpha");
    }

    TEST_CASE("a constant the shader never declares is empty") {
        auto info = shaderWithAlias({ { "x", "g_Alpha" } });
        CHECK(wek::ConstantUniformName(info, "Beta").empty());
    }

    TEST_CASE("a one-character alias neither throws nor matches") {
        auto        info = shaderWithAlias({ { "m", "x" } });
        std::string got  = "unset";
        CHECK_NOTHROW(got = wek::ConstantUniformName(info, "x"));
        CHECK(got.empty());
    }

    TEST_CASE("an alias that is only the prefix does not match an empty name") {
        auto info = shaderWithAlias({ { "m", "g_" } });
        CHECK(wek::ConstantUniformName(info, "").empty());
    }

    TEST_CASE("the user binding name falls back to the raw constant name") {
        auto info = shaderWithAlias({ { "x", "g_Alpha" } });
        CHECK(wek::UserBindingUniformName(info, "Beta") == "Beta");
    }

    TEST_CASE("the user binding name agrees with the constant name when the shader declares it") {
        auto info = shaderWithAlias({ { "alpha", "g_Alpha" }, { "y", "g_Tint" } });
        CHECK(wek::UserBindingUniformName(info, "alpha") == "g_Alpha");
        CHECK(wek::UserBindingUniformName(info, "Tint") == "g_Tint");
    }

    TEST_CASE("register records the resolved uniform under the property name") {
        Scene scene;
        auto  mesh = meshWithMaterial();
        wek::registerUserPropUniformBinding(scene, "prop", mesh, "g_Alpha");

        REQUIRE(scene.userPropUniformBindings.count("prop") == 1u);
        REQUIRE(scene.userPropUniformBindings.at("prop").size() == 1u);
        CHECK(scene.userPropUniformBindings.at("prop")[0].uniformName == "g_Alpha");
    }

    TEST_CASE("apply writes a bound uniform and marks the material dirty") {
        Scene scene;
        auto  mesh = meshWithMaterial();
        wek::registerUserPropUniformBinding(scene, "prop", mesh, "g_Alpha");

        applyAndDrain(scene, nlohmann::json { { "prop", 0.5 } });

        auto* mat = mesh->Material();
        REQUIRE(mat->customShader.constValues.count("g_Alpha") == 1u);
        CHECK(mat->customShader.constValues.at("g_Alpha")[0] == doctest::Approx(0.5f));
        CHECK(mat->customShader.constValuesDirty);
    }

    TEST_CASE("two bindings on one property both receive the value") {
        Scene scene;
        auto  a = meshWithMaterial();
        auto  b = meshWithMaterial();
        wek::registerUserPropUniformBinding(scene, "prop", a, "g_Alpha");
        wek::registerUserPropUniformBinding(scene, "prop", b, "g_Strength");

        applyAndDrain(scene, nlohmann::json { { "prop", 2.0 } });

        CHECK(a->Material()->customShader.constValues.at("g_Alpha")[0] == doctest::Approx(2.0f));
        CHECK(b->Material()->customShader.constValues.at("g_Strength")[0] == doctest::Approx(2.0f));
    }

    TEST_CASE("a property absent from the update leaves the material untouched") {
        Scene scene;
        auto  mesh = meshWithMaterial();
        wek::registerUserPropUniformBinding(scene, "prop", mesh, "g_Alpha");

        applyAndDrain(scene, nlohmann::json { { "other", 1.0 } });

        CHECK(mesh->Material()->customShader.constValues.count("g_Alpha") == 0u);
        CHECK_FALSE(mesh->Material()->customShader.constValuesDirty);
    }

    TEST_CASE("a value that parses to nothing writes nothing") {
        Scene scene;
        auto  mesh = meshWithMaterial();
        wek::registerUserPropUniformBinding(scene, "prop", mesh, "g_Alpha");

        applyAndDrain(scene, nlohmann::json { { "prop", nlohmann::json::object() } });

        CHECK(mesh->Material()->customShader.constValues.count("g_Alpha") == 0u);
        CHECK_FALSE(mesh->Material()->customShader.constValuesDirty);
    }

    TEST_CASE("a binding keeps its dropped effect's mesh alive") {
        // An effect dropped after its binding was recorded (a later pass failed
        // to load, or RemoveFailedEffects reaped it) must not leave the binding
        // writing through a freed material.
        Scene                    scene;
        auto                     mesh = meshWithMaterial();
        std::weak_ptr<SceneMesh> weak = mesh;
        wek::registerUserPropUniformBinding(scene, "prop", mesh, "g_Alpha");

        mesh.reset(); // the effect is dropped; nothing else owns the mesh
        REQUIRE_FALSE(weak.expired());

        applyAndDrain(scene, nlohmann::json { { "prop", 0.5 } });

        auto kept = weak.lock();
        REQUIRE(kept);
        CHECK(kept->Material()->customShader.constValues.at("g_Alpha")[0] == doctest::Approx(0.5f));
        CHECK(kept->Material()->customShader.constValuesDirty);
    }

    TEST_CASE("a binding whose mesh has no material is skipped") {
        Scene scene;
        auto  empty = std::make_shared<SceneMesh>();
        auto  live  = meshWithMaterial();
        scene.userPropUniformBindings["prop"].push_back({ empty, "g_Alpha" });
        wek::registerUserPropUniformBinding(scene, "prop", live, "g_Alpha");

        CHECK_NOTHROW(applyAndDrain(scene, nlohmann::json { { "prop", 0.5 } }));

        CHECK(live->Material()->customShader.constValues.at("g_Alpha")[0] == doctest::Approx(0.5f));
    }

    TEST_CASE("applying a user prop leaves the material alone until the render thread drains") {
        Scene               scene;
        auto                mesh = meshWithMaterial();
        PendingUpdateQueues q;
        wek::registerUserPropUniformBinding(scene, "prop", mesh, "g_Alpha");

        wek::applyUserPropUniforms(scene, nlohmann::json { { "prop", 0.5 } }, q);

        auto* mat = mesh->Material();
        CHECK(mat->customShader.constValues.count("g_Alpha") == 0u);
        CHECK_FALSE(mat->customShader.constValuesDirty);

        q.withPropertyLocked([](PendingUpdateQueues& p) {
            wek::drainUserPropUniforms(p.m_pending_user_prop_uniforms);
        });
        REQUIRE(mat->customShader.constValues.count("g_Alpha") == 1u);
        CHECK(mat->customShader.constValues.at("g_Alpha")[0] == doctest::Approx(0.5f));
        CHECK(mat->customShader.constValuesDirty);
    }

    TEST_CASE("a user value survives a finished single-shot curve and loses to a looping one") {
        auto fade = [](PropertyAnimMode mode) {
            PropertyAnimation a;
            a.mode      = mode;
            a.fps       = 1.0f;
            a.length    = 2.0f;
            a.keyframes = { { 0.0f, 1.0f }, { 2.0f, 0.0f } };
            a.playing   = true;
            return a;
        };
        auto post = [](const std::shared_ptr<SceneMesh>& mesh) {
            std::vector<PendingUpdateQueues::UserPropUniformUpdate> pending;
            pending.push_back({ mesh, "g_Alpha", { 0.5f } });
            wek::drainUserPropUniforms(pending);
        };

        {
            auto                                       mesh = meshWithMaterial();
            std::vector<Scene::EffectUniformAnimation> anims;
            anims.push_back(
                { mesh, "g_Alpha", { { 1.0f }, { { 0, fade(PropertyAnimMode::Single) } } } });
            TickEffectUniformAnimations(anims, 5.0); // runs to its end and holds
            post(mesh);
            TickEffectUniformAnimations(anims, 1.0);
            CHECK(mesh->Material()->customShader.constValues.at("g_Alpha")[0] ==
                  doctest::Approx(0.5f));
        }
        {
            auto                                       mesh = meshWithMaterial();
            std::vector<Scene::EffectUniformAnimation> anims;
            anims.push_back(
                { mesh, "g_Alpha", { { 1.0f }, { { 0, fade(PropertyAnimMode::Loop) } } } });
            TickEffectUniformAnimations(anims, 1.0);
            post(mesh);
            TickEffectUniformAnimations(anims, 0.5);
            CHECK(mesh->Material()->customShader.constValues.at("g_Alpha")[0] !=
                  doctest::Approx(0.5f));
        }
    }
}
