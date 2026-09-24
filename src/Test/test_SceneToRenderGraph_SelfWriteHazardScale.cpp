#include <doctest.h>

#include "VulkanRender/SceneToRenderGraph.hpp"
#include "VulkanRender/CopyPass.hpp"
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneNode.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneMaterial.h"
#include "SpecTexs.hpp"
#include "Core/StringHelper.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace wallpaper;

namespace
{

std::shared_ptr<SceneNode> makeHazardNode(const std::string& key, const std::string& name) {
    auto          node = std::make_shared<SceneNode>();
    auto          mesh = std::make_shared<SceneMesh>();
    SceneMaterial mat;
    mat.name     = name;
    mat.textures = { key };
    mesh->AddMaterial(std::move(mat));
    node->AddMesh(mesh);
    return node;
}

// Every hazard copy this branch creates gets a generated "<src>_<version>_copy"
// destination key (SceneToRenderGraph.cpp:46-59); this file's other addCopyPass
// call sites (bloom scene-capture, link-texture resolution) pass an explicit
// fixed dst that never carries that suffix, so filtering on it isolates hazard
// copies from unrelated ones that happen to share a source key.
std::vector<std::string> hazardCopyDstKeys(rg::RenderGraph& rgraph, std::string_view src_key) {
    std::vector<std::string> keys;
    for (auto id : rgraph.topologicalOrder()) {
        auto* cp = dynamic_cast<vulkan::CopyPass*>(rgraph.getPass(id));
        if (cp == nullptr) continue;
        if (cp->desc().src == src_key && send_with(cp->desc().dst, "_copy"))
            keys.push_back(cp->desc().dst);
    }
    return keys;
}

} // namespace

TEST_SUITE("SceneToRenderGraph_SelfWriteHazardScale") {
    TEST_CASE("a self-write hazard copy is registered as a scaled render target, not left for "
              "CopyPass::prepare's full-size clone fallback") {
        Scene scene;
        scene.renderTargets[std::string(SpecTex_Default)] = SceneRenderTarget {
            .width  = 3840,
            .height = 2160,
            .bind   = { .enable = true, .screen = true },
        };
        scene.sceneGraph->AppendChild(
            makeHazardNode(std::string(SpecTex_Default), "refract_particle"));

        auto rgraph = wallpaper::sceneToRenderGraph(scene);
        auto keys   = hazardCopyDstKeys(*rgraph, SpecTex_Default);
        REQUIRE(keys.size() == 1);

        REQUIRE(scene.renderTargets.count(keys[0]) > 0);
        const auto& copy_rt = scene.renderTargets.at(keys[0]);
        CHECK(copy_rt.bind.enable == true);
        CHECK(copy_rt.bind.screen == false);
        CHECK(copy_rt.bind.name == std::string(SpecTex_Default));
        CHECK(copy_rt.bind.scale < 1.0);
    }

    TEST_CASE("the hazard copy's entry clones format/withDepth/has_mipmap from the source, not "
              "just size") {
        Scene scene;
        scene.renderTargets[std::string(SpecTex_Default)] = SceneRenderTarget {
            .width      = 3840,
            .height     = 2160,
            .withDepth  = true,
            .has_mipmap = true,
            .format     = TextureFormat::RGBA16F,
            .bind       = { .enable = true, .screen = true },
        };
        scene.sceneGraph->AppendChild(
            makeHazardNode(std::string(SpecTex_Default), "refract_particle"));

        auto rgraph = wallpaper::sceneToRenderGraph(scene);
        auto keys   = hazardCopyDstKeys(*rgraph, SpecTex_Default);
        REQUIRE(keys.size() == 1);
        REQUIRE(scene.renderTargets.count(keys[0]) > 0);

        const auto& copy_rt = scene.renderTargets.at(keys[0]);
        CHECK(copy_rt.format == TextureFormat::RGBA16F);
        CHECK(copy_rt.withDepth == true);
        CHECK(copy_rt.has_mipmap == true);
    }

    TEST_CASE("hazard copies against two different source targets get independently scaled "
              "entries, not collapsed into one") {
        Scene scene;
        scene.renderTargets[std::string(SpecTex_Default)] = SceneRenderTarget {
            .width  = 3840,
            .height = 2160,
            .bind   = { .enable = true, .screen = true },
        };
        const std::string bloom_key(WE_BLOOM_MIP1);
        scene.renderTargets[bloom_key] = SceneRenderTarget {
            .width  = 1920,
            .height = 1080,
            .bind   = { .enable = true, .screen = true },
        };
        scene.sceneGraph->AppendChild(
            makeHazardNode(std::string(SpecTex_Default), "hazard_default"));

        scene.bloomConfig.enabled = true;
        auto bloom_node           = makeHazardNode(bloom_key, "hazard_bloom");
        scene.bloomConfig.nodes.push_back(bloom_node);
        scene.bloomConfig.outputs.push_back(bloom_key);

        auto rgraph       = wallpaper::sceneToRenderGraph(scene);
        auto default_keys = hazardCopyDstKeys(*rgraph, SpecTex_Default);
        auto bloom_keys   = hazardCopyDstKeys(*rgraph, bloom_key);
        REQUIRE(default_keys.size() == 1);
        REQUIRE(bloom_keys.size() == 1);
        REQUIRE(scene.renderTargets.count(default_keys[0]) > 0);
        REQUIRE(scene.renderTargets.count(bloom_keys[0]) > 0);

        CHECK(scene.renderTargets.at(default_keys[0]).bind.name == std::string(SpecTex_Default));
        CHECK(scene.renderTargets.at(bloom_keys[0]).bind.name == bloom_key);
    }

    TEST_CASE("two self-write hazards against the SAME source in one build each get their own "
              "distinct, correctly scaled entry") {
        Scene scene;
        scene.renderTargets[std::string(SpecTex_Default)] = SceneRenderTarget {
            .width  = 3840,
            .height = 2160,
            .bind   = { .enable = true, .screen = true },
        };
        scene.sceneGraph->AppendChild(
            makeHazardNode(std::string(SpecTex_Default), "refract_particle_0"));
        scene.sceneGraph->AppendChild(
            makeHazardNode(std::string(SpecTex_Default), "refract_particle_1"));

        auto rgraph = wallpaper::sceneToRenderGraph(scene);
        auto keys   = hazardCopyDstKeys(*rgraph, SpecTex_Default);
        REQUIRE(keys.size() == 2);
        CHECK(keys[0] != keys[1]);

        for (auto& key : keys) {
            REQUIRE(scene.renderTargets.count(key) > 0);
            const auto& copy_rt = scene.renderTargets.at(key);
            CHECK(copy_rt.bind.enable == true);
            CHECK(copy_rt.bind.screen == false);
            CHECK(copy_rt.bind.name == std::string(SpecTex_Default));
            CHECK(copy_rt.bind.scale < 1.0);
        }
    }

} // TEST_SUITE
