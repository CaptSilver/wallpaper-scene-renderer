#include "WPSceneParser.hpp"
#include "WPJson.hpp"
#include "WPUserProperties.hpp"

#include "Utils/String.h"
#include "Utils/Logging.h"
#include "Utils/Algorism.h"
#include "Core/Visitors.hpp"
#include "Core/StringHelper.hpp"
#include "Core/ArrayHelper.hpp"
#include "SpecTexs.hpp"

#include "WPShaderParser.hpp"
#include "WPTexImageParser.hpp"
#include "WPParticleParser.hpp"
#include "WPSoundParser.hpp"
#include "WPMdlParser.hpp"

#include "Particle/WPParticleRawGener.h"
#include "Particle/ParticleSystem.h"

#include "WPShaderValueUpdater.hpp"
#include "wpscene/WPImageObject.h"
#include "wpscene/WPParticleObject.h"
#include "wpscene/WPSoundObject.h"
#include "wpscene/WPLightObject.hpp"
#include "wpscene/WPModelObject.h"
#include "wpscene/WPTextObject.h"
#include "wpscene/WPScene.h"

#include "WPTextRenderer.hpp"

#include "Fs/VFS.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <random>
#include <cmath>
#include <functional>
#include <regex>
#include <variant>
#include <Eigen/Dense>

using namespace wallpaper;
using namespace Eigen;

std::string getAddr(void* p) { return std::to_string(reinterpret_cast<intptr_t>(p)); }

struct ParseContext {
    std::shared_ptr<Scene> scene;
    WPShaderValueUpdater*  shader_updater;
    i32                    ortho_w;
    i32                    ortho_h;
    fs::VFS*               vfs;

    ShaderValueMap             global_base_uniforms;
    std::shared_ptr<SceneNode> effect_camera_node;
    std::shared_ptr<SceneNode> global_camera_node;
    std::shared_ptr<SceneNode> global_perspective_camera_node;

    // Map of object id → SceneNode for parent-child hierarchy (group nodes + parsed objects)
    std::map<i32, std::shared_ptr<SceneNode>> node_map;
};

using WPObjectVar = std::variant<wpscene::WPImageObject, wpscene::WPParticleObject,
                                 wpscene::WPSoundObject, wpscene::WPLightObject,
                                 wpscene::WPModelObject, wpscene::WPTextObject>;

namespace
{
// mapRate < 1.0
void GenCardMesh(SceneMesh& mesh, const std::array<uint16_t, 2> size,
                 const std::array<float, 2> mapRate = { 1.0f, 1.0f }) {
    float left   = -(size[0] / 2.0f);
    float right  = size[0] / 2.0f;
    float bottom = -(size[1] / 2.0f);
    float top    = size[1] / 2.0f;
    float z      = 0.0f;

    float tw = mapRate[0], th = mapRate[1];

    // clang-format off
	const std::array pos = {
		left, bottom, z,
		left,  top, z,
		right, bottom, z,
		right,  top, z,
	};
	const std::array texCoord = {
		0.0f, th,
		0.0f, 0.0f,
		tw, th,
		tw, 0.0f,
	};
    // clang-format on

    SceneVertexArray vertex(
        {
            { WE_IN_POSITION.data(), VertexType::FLOAT3 },
            { WE_IN_TEXCOORD.data(), VertexType::FLOAT2 },
        },
        4);
    vertex.SetVertex(WE_IN_POSITION, pos);
    vertex.SetVertex(WE_IN_TEXCOORD, texCoord);
    mesh.AddVertexArray(std::move(vertex));
}

void SetParticleMesh(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
                     bool thick_format) {
    (void)particle;
    std::vector<SceneVertexArray::SceneVertexAttribute> attrs {
        { WE_IN_POSITION.data(), VertexType::FLOAT3 },
        { WE_IN_TEXCOORDVEC4.data(), VertexType::FLOAT4 },
        { WE_IN_COLOR.data(), VertexType::FLOAT4 },
    };
    if (thick_format) {
        attrs.push_back({ WE_IN_TEXCOORDVEC4C1.data(), VertexType::FLOAT4 });
    }
    attrs.push_back({ WE_IN_TEXCOORDC2.data(), VertexType::FLOAT2 });
    mesh.AddVertexArray(SceneVertexArray(attrs, count * 4));
    mesh.AddIndexArray(SceneIndexArray(count));
    mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
}

void SetRopeParticleMesh(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
                         bool thick_format) {
    (void)particle;
    std::vector<SceneVertexArray::SceneVertexAttribute> attrs {
        { WE_IN_POSITIONVEC4.data(), VertexType::FLOAT4 },
        { WE_IN_TEXCOORDVEC4.data(), VertexType::FLOAT4 },
        { WE_IN_TEXCOORDVEC4C1.data(), VertexType::FLOAT4 },
    };
    if (thick_format) {
        attrs.push_back({ WE_IN_TEXCOORDVEC4C2.data(), VertexType::FLOAT4 });
        attrs.push_back({ WE_IN_TEXCOORDVEC4C3.data(), VertexType::FLOAT4 });
        attrs.push_back({ WE_IN_TEXCOORDC4.data(), VertexType::FLOAT4 });
    } else {
        attrs.push_back({ WE_IN_TEXCOORDVEC3C2.data(), VertexType::FLOAT4 });
        attrs.push_back({ WE_IN_TEXCOORDC3.data(), VertexType::FLOAT4 });
    }
    attrs.push_back({ WE_IN_COLOR.data(), VertexType::FLOAT4 });
    mesh.AddVertexArray(SceneVertexArray(attrs, count * 4));
    mesh.AddIndexArray(SceneIndexArray(count));
    mesh.GetVertexArray(0).SetOption(WE_PRENDER_ROPE, true);
    mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
}

ParticleAnimationMode ToAnimMode(const std::string& str) {
    if (str == "randomframe")
        return ParticleAnimationMode::RANDOMONE;
    else if (str == "sequence")
        return ParticleAnimationMode::SEQUENCE;
    else {
        return ParticleAnimationMode::SEQUENCE;
    }
}

void LoadControlPoint(ParticleSubSystem& pSys, const wpscene::Particle& wp) {
    std::span<ParticleControlpoint> pcs = pSys.Controlpoints();
    usize                           s   = std::min(pcs.size(), wp.controlpoints.size());
    for (usize i = 0; i < s; i++) {
        pcs[i].offset = Eigen::Vector3d { array_cast<double>(wp.controlpoints[i].offset).data() };
        pcs[i].link_mouse =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::link_mouse];
        pcs[i].worldspace =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::worldspace];
    }
}
void LoadInitializer(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                     const wpscene::ParticleInstanceoverride& over) {
    for (const auto& ini : wp.initializers) {
        pSys.AddInitializer(WPParticleParser::genParticleInitOp(ini));
    }
    if (over.enabled) pSys.AddInitializer(WPParticleParser::genOverrideInitOp(over));
}
void LoadOperator(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                  const wpscene::ParticleInstanceoverride& over) {
    for (const auto& op : wp.operators) {
        pSys.AddOperator(WPParticleParser::genParticleOperatorOp(op, over));
    }
}
void LoadEmitter(ParticleSubSystem& pSys, const wpscene::Particle& wp, float count,
                 bool render_rope) {
    bool sort = render_rope;
    for (const auto& em : wp.emitters) {
        auto newEm = em;
        newEm.rate *= count;
        // newEm.origin[2] -= perspectiveZ;
        pSys.AddEmitter(WPParticleParser::genParticleEmittOp(newEm, sort));
    }
}

ParticleSubSystem::SpawnType ParseSpawnType(std::string_view str) {
    using ST = ParticleSubSystem::SpawnType;
    ST type { ST::STATIC };
    if (str == "eventfollow") {
        type = ST::EVENT_FOLLOW;
    } else if (str == "eventspawn") {
        type = ST::EVENT_SPAWN;
    } else if (str == "eventdeath") {
        type = ST::EVENT_DEATH;
    }
    return type;
};

BlendMode ParseBlendMode(std::string_view str) {
    BlendMode bm;
    if (str == "translucent") {
        bm = BlendMode::Translucent;
    } else if (str == "additive") {
        bm = BlendMode::Additive;
    } else if (str == "normal") {
        bm = BlendMode::Normal;
    } else if (str == "disabled") {
        // seems disabled is normal
        bm = BlendMode::Normal;
    } else {
        bm = BlendMode::Normal;
        LOG_ERROR("unknown blending: %s", str.data());
    }
    return bm;
}

void ParseSpecTexName(std::string& name, const wpscene::WPMaterial& wpmat,
                      const WPShaderInfo& sinfo, const Scene* pScene = nullptr) {
    if (IsSpecTex(name)) {
        if (name == "_rt_FullFrameBuffer") {
            name = SpecTex_Default;
            if (wpmat.shader == "genericimage2" && ! exists(sinfo.combos, "BLENDMODE")) name = "";
            /*
            if(wpmat.shader == "genericparticle") {
                name = "_rt_ParticleRefract";
            }
            */
        } else if (sstart_with(name, WE_IMAGE_LAYER_COMPOSITE_PREFIX)) {
            LOG_INFO("link tex \"%s\"", name.c_str());
            int         wpid { -1 };
            std::regex  reImgId { R"(_rt_imageLayerComposite_([0-9]+))" };
            std::smatch match;
            if (std::regex_search(name, match, reImgId)) {
                STRTONUM(std::string(match[1]), wpid);
            }
            name = GenLinkTex((u32)wpid);
        } else if (sstart_with(name, WE_MIP_MAPPED_FRAME_BUFFER)) {
        } else if (sstart_with(name, WE_EFFECT_PPONG_PREFIX)) {
        } else if (sstart_with(name, WE_HALF_COMPO_BUFFER_PREFIX)) {
        } else if (sstart_with(name, WE_QUARTER_COMPO_BUFFER_PREFIX)) {
        } else if (sstart_with(name, WE_FULL_COMPO_BUFFER_PREFIX)) {
        } else if (name == WE_SHADOW_ATLAS) {
        } else if (name == WE_REFLECTION) {
        } else if (sstart_with(name, WE_BUFFER_PREFIX)) {
        } else if (sstart_with(name, WE_BLOOM_PREFIX)) {
        } else if (pScene && pScene->renderTargets.count(name) > 0) {
            // Dynamic effect FBO — already registered as a render target
        } else {
            LOG_ERROR("unknown tex \"%s\"", name.c_str());
        }
    }
}

bool LoadMaterial(fs::VFS& vfs, const wpscene::WPMaterial& wpmat, Scene* pScene, SceneNode* pNode,
                  SceneMaterial* pMaterial, WPShaderValueData* pSvData,
                  WPShaderInfo* pWPShaderInfo = nullptr) {
    (void)pNode;

    auto& svData   = *pSvData;
    auto& material = *pMaterial;

    std::unique_ptr<WPShaderInfo> upWPShaderInfo(nullptr);
    if (pWPShaderInfo == nullptr) {
        upWPShaderInfo = std::make_unique<WPShaderInfo>();
        pWPShaderInfo  = upWPShaderInfo.get();
    }

    SceneMaterialCustomShader materialShader;

    auto& shader = materialShader.shader;
    shader       = std::make_shared<SceneShader>();
    shader->name = wpmat.shader;

    std::string shaderPath("/assets/shaders/" + wpmat.shader);

    std::array sd_units { WPShaderUnit {
                              .stage           = ShaderType::VERTEX,
                              .src             = fs::GetFileContent(vfs, shaderPath + ".vert"),
                              .preprocess_info = {},
                          },
                          WPShaderUnit {
                              .stage           = ShaderType::FRAGMENT,
                              .src             = fs::GetFileContent(vfs, shaderPath + ".frag"),
                              .preprocess_info = {},
                          } };

    std::vector<WPShaderTexInfo>                 texinfos;
    std::unordered_map<std::string, ImageHeader> texHeaders;
    for (const auto& el : wpmat.textures) {
        if (el.empty()) {
            texinfos.push_back({ false });
        } else if (! IsSpecTex(el)) {
            const auto& texh = pScene->imageParser->ParseHeader(el);
            texHeaders[el]   = texh;
            if (texh.extraHeader.count("compo1") == 0) {
                texinfos.push_back({ false });
                continue;
            }
            texinfos.push_back({ true,
                                 {
                                     (bool)texh.extraHeader.at("compo1").val,
                                     (bool)texh.extraHeader.at("compo2").val,
                                     (bool)texh.extraHeader.at("compo3").val,
                                 } });
        } else
            texinfos.push_back({ true });
    }

    for (auto& unit : sd_units) {
        unit.src = WPShaderParser::PreShaderSrc(vfs, unit.src, pWPShaderInfo, texinfos);
    }

    shader->default_uniforms = pWPShaderInfo->svs;

    for (const auto& el : wpmat.combos) {
        pWPShaderInfo->combos[el.first] = std::to_string(el.second);
    }

    auto textures = wpmat.textures;
    if (pWPShaderInfo->defTexs.size() > 0) {
        for (auto& t : pWPShaderInfo->defTexs) {
            if (textures.size() > t.first) {
                if (! textures.at(t.first).empty()) continue;
            } else {
                textures.resize(t.first + 1);
            }
            textures[t.first] = t.second;
        }
    }

    for (usize i = 0; i < textures.size(); i++) {
        std::string name = textures.at(i);
        ParseSpecTexName(name, wpmat, *pWPShaderInfo, pScene);
        material.textures.push_back(name);
        material.defines.push_back("g_Texture" + std::to_string(i));
        if (name.empty()) {
            continue;
        }

        std::array<i32, 4> resolution {};
        if (IsSpecTex(name)) {
            if (IsSpecLinkTex(name)) {
                svData.renderTargets.push_back({ i, name });
                // Link textures reference offscreen dependency nodes.
                // Set resolution from the linked node's render target so
                // vertex shaders can compute correct UV scaling (without
                // this, resolution stays at 0 and UV = 0/0 = NaN).
                auto linkId        = ParseLinkTex(name);
                auto offscreenName = GenOffscreenRT(linkId);
                if (pScene->renderTargets.count(offscreenName) > 0) {
                    const auto& rt = pScene->renderTargets.at(offscreenName);
                    resolution     = { rt.width, rt.height, rt.width, rt.height };
                    LOG_INFO("  link tex[%zu] '%s' → '%s' resolution=(%d,%d,%d,%d)",
                             i, name.c_str(), offscreenName.c_str(),
                             rt.width, rt.height, rt.width, rt.height);
                } else if (pScene->renderTargets.count(std::string(SpecTex_Default)) > 0) {
                    const auto& rt =
                        pScene->renderTargets.at(std::string(SpecTex_Default));
                    resolution = { rt.width, rt.height, rt.width, rt.height };
                    LOG_INFO("  link tex[%zu] '%s' → _rt_default resolution=(%d,%d,%d,%d)",
                             i, name.c_str(), rt.width, rt.height, rt.width, rt.height);
                } else {
                    LOG_ERROR("  link tex[%zu] '%s' → '%s' NOT FOUND",
                              i, name.c_str(), offscreenName.c_str());
                }
            } else if (pScene->renderTargets.count(name) == 0) {
                LOG_ERROR("%s not found in render targets", name.c_str());
            } else {
                svData.renderTargets.push_back({ i, name });
                const auto& rt = pScene->renderTargets.at(name);
                resolution     = { rt.width, rt.height, rt.width, rt.height };
            }
        } else {
            const ImageHeader& texh = texHeaders.count(name) == 0
                                          ? pScene->imageParser->ParseHeader(name)
                                          : texHeaders.at(name);
            if (i == 0) {
                if (texh.format == TextureFormat::R8)
                    pWPShaderInfo->combos["TEX0FORMAT"] = "FORMAT_R8";
                else if (texh.format == TextureFormat::RG8)
                    pWPShaderInfo->combos["TEX0FORMAT"] = "FORMAT_RG88";
            }
            if (texh.mipmap_larger) {
                resolution = { texh.width, texh.height, texh.mapWidth, texh.mapHeight };
            } else {
                resolution = { texh.mapWidth, texh.mapHeight, texh.mapWidth, texh.mapHeight };
            }

            if (pScene->textures.count(name) == 0) {
                SceneTexture stex;
                stex.sample = texh.sample;
                stex.url    = name;
                if (texh.isSprite) {
                    stex.isSprite   = texh.isSprite;
                    stex.spriteAnim = texh.spriteAnim;
                }
                pScene->textures[name] = stex;
            }
            if ((pScene->textures.at(name)).isSprite) {
                material.hasSprite = true;
                const auto& f1     = texh.spriteAnim.GetCurFrame();
                if (wpmat.shader == "genericparticle" || wpmat.shader == "genericropeparticle") {
                    pWPShaderInfo->combos["SPRITESHEET"] = "1";
                    pWPShaderInfo->combos["THICKFORMAT"] = "1";
                    if (algorism::IsPowOfTwo((u32)texh.width) &&
                        algorism::IsPowOfTwo((u32)texh.height)) {
                        pWPShaderInfo->combos["SPRITESHEETBLENDNPOT"] = "1";
                        resolution[2] = resolution[0] - resolution[0] % (int)f1.width;
                        resolution[3] = resolution[1] - resolution[1] % (int)f1.height;
                    }
                    materialShader.constValues["g_RenderVar1"] = std::array {
                        f1.xAxis[0], f1.yAxis[1], (float)(texh.spriteAnim.numFrames()), f1.rate
                    };
                }
            }
        }
        if (! resolution.empty()) {
            const std::string gResolution = WE_GLTEX_RESOLUTION_NAMES[i];

            materialShader.constValues[gResolution] = array_cast<float>(resolution);
        }
    }
    if (exists(pWPShaderInfo->combos, "LIGHTING")) {
        // pWPShaderInfo->combos["PRELIGHTING"] =
        // pWPShaderInfo->combos.at("LIGHTING");
    }

    if (! WPShaderParser::CompileToSpv(
            pScene->scene_id, sd_units, shader->codes, vfs, pWPShaderInfo, texinfos)) {
        return false;
    }

    material.blenmode   = ParseBlendMode(wpmat.blending);
    material.depthTest  = (wpmat.depthtest == "enabled");
    material.depthWrite = (wpmat.depthwrite == "enabled");
    material.cullmode   = wpmat.cullmode;

    for (uint i = 0; i < material.textures.size(); i++) {
        if (! exists(sd_units[1].preprocess_info.active_tex_slots, i)) material.textures[i].clear();
    }

    for (const auto& el : pWPShaderInfo->baseConstSvs) {
        materialShader.constValues[el.first] = el.second;
    }
    material.customShader = materialShader;
    material.name         = wpmat.shader;

    return true;
}

void LoadAlignment(SceneNode& node, std::string_view align, Vector2f size) {
    Vector3f trans = node.Translate();
    size *= 0.5f;
    size.y() *= 1.0f;

    auto contains = [&](std::string_view s) {
        return align.find(s) != std::string::npos;
    };

    // topleft top center ...
    if (contains("top")) trans.y() -= size.y();
    if (contains("left")) trans.x() += size.x();
    if (contains("right")) trans.x() -= size.x();
    if (contains("bottom")) trans.y() += size.y();

    node.SetTranslate(trans);
}

void LoadConstvalue(SceneMaterial& material, const wpscene::WPMaterial& wpmat,
                    const WPShaderInfo& info) {
    // load glname from alias and load to constvalue
    for (const auto& cs : wpmat.constantshadervalues) {
        const auto&               name  = cs.first;
        const std::vector<float>& value = cs.second;
        std::string               glname;
        if (info.alias.count(name) != 0) {
            glname = info.alias.at(name);
        } else {
            for (const auto& el : info.alias) {
                if (el.second.substr(2) == name) {
                    glname = el.second;
                    break;
                }
            }
        }
        if (glname.empty()) {
            LOG_ERROR("ShaderValue: %s not found in glsl", name.c_str());
        } else {
            material.customShader.constValues[glname] = value;
            if (value.size() <= 4) {
                std::string valStr;
                for (size_t i = 0; i < value.size(); i++) {
                    if (i > 0) valStr += ",";
                    valStr += std::to_string(value[i]);
                }
                LOG_INFO("  constValue: '%s' -> '%s' = [%s]", name.c_str(), glname.c_str(), valStr.c_str());
            }
        }
    }
}

// parse

void ParseCamera(ParseContext& context, wpscene::WPScene& sc) {
    auto& general = sc.general;
    auto& scene   = *context.scene;

    // effect camera (always 2x2 ortho for post-processing passes)
    scene.cameras["effect"]    = std::make_shared<SceneCamera>(2, 2, -1.0f, 1.0f);
    context.effect_camera_node = std::make_shared<SceneNode>(); // at 0,0,0
    scene.cameras.at("effect")->AttatchNode(context.effect_camera_node);
    scene.sceneGraph->AppendChild(context.effect_camera_node);

    if (! general.isOrtho) {
        // 3D perspective scene — use eye/center/up from scene camera
        float aspect = (float)context.ortho_w / (float)context.ortho_h;
        scene.cameras["global"] =
            std::make_shared<SceneCamera>(aspect, general.nearz, general.farz, general.fov);
        scene.activeCamera = scene.cameras.at("global").get();

        Vector3d eye(sc.camera.eye[0], sc.camera.eye[1], sc.camera.eye[2]);
        Vector3d center(sc.camera.center[0], sc.camera.center[1], sc.camera.center[2]);
        Vector3d up(sc.camera.up[0], sc.camera.up[1], sc.camera.up[2]);
        scene.activeCamera->SetDirectLookAt(eye, center, up);

        LOG_INFO("Perspective camera: eye=(%.3f,%.3f,%.3f) center=(%.3f,%.3f,%.3f) "
                 "fov=%.1f aspect=%.3f near=%.3f far=%.1f",
                 sc.camera.eye[0], sc.camera.eye[1], sc.camera.eye[2],
                 sc.camera.center[0], sc.camera.center[1], sc.camera.center[2],
                 general.fov, aspect, general.nearz, general.farz);

        // Load camera animation paths
        auto& vfs = *context.vfs;
        std::vector<CameraPath> camPaths;
        for (auto& pathFile : sc.camera.paths) {
            std::string content = fs::GetFileContent(vfs, "/assets/" + pathFile);
            if (content.empty()) {
                LOG_ERROR("failed to load camera path file: %s", pathFile.c_str());
                continue;
            }
            nlohmann::json jPaths;
            if (! PARSE_JSON(content, jPaths)) continue;
            if (! jPaths.contains("paths") || ! jPaths.at("paths").is_array()) continue;

            for (auto& jPath : jPaths.at("paths")) {
                CameraPath cp;
                cp.duration = jPath.value("duration", 0.0);
                if (jPath.contains("transforms") && jPath.at("transforms").is_array()) {
                    for (auto& jT : jPath.at("transforms")) {
                        CameraKeyframe kf;
                        std::array<float, 3> kf_eye, kf_center, kf_up;
                        GET_JSON_NAME_VALUE(jT, "eye", kf_eye);
                        GET_JSON_NAME_VALUE(jT, "center", kf_center);
                        GET_JSON_NAME_VALUE(jT, "up", kf_up);
                        kf.eye       = Vector3d(kf_eye[0], kf_eye[1], kf_eye[2]);
                        kf.center    = Vector3d(kf_center[0], kf_center[1], kf_center[2]);
                        kf.up        = Vector3d(kf_up[0], kf_up[1], kf_up[2]);
                        kf.timestamp = jT.value("timestamp", 0.0);
                        cp.keyframes.push_back(kf);
                    }
                }
                if (cp.keyframes.size() >= 2 && cp.duration > 0) {
                    camPaths.push_back(std::move(cp));
                }
            }
            LOG_INFO("loaded %zu camera paths from %s", camPaths.size(), pathFile.c_str());
        }
        if (! camPaths.empty()) {
            // Create reflected camera for planar reflections (Y=0 mirror).
            // Must be done BEFORE LoadPaths moves the vector.
            auto reflCam = std::make_shared<SceneCamera>(
                aspect, general.nearz, general.farz, general.fov);
            reflCam->SetReflectY0(true);
            reflCam->SetDirectLookAt(eye, center, up);
            reflCam->LoadPaths(camPaths); // copy paths (not move)
            scene.cameras["reflected_perspective"] = reflCam;

            scene.activeCamera->LoadPaths(std::move(camPaths));
        }
    } else {
        // 2D orthographic scene (existing path)
        scene.cameras["global"] =
            std::make_shared<SceneCamera>((context.ortho_w / (i32)general.zoom),
                                          (context.ortho_h / (i32)general.zoom),
                                          -5000.0f,
                                          5000.0f);
        scene.activeCamera = scene.cameras.at("global").get();
        Vector3f cori { (float)context.ortho_w / 2.0f, (float)context.ortho_h / 2.0f, 0 },
            cscale { 1.0f, 1.0f, 1.0f }, cangle(Vector3f::Zero());

        context.global_camera_node = std::make_shared<SceneNode>(cori, cscale, cangle);
        scene.activeCamera->AttatchNode(context.global_camera_node);
        scene.sceneGraph->AppendChild(context.global_camera_node);
        LOG_INFO("Global camera: %dx%d at (%.1f, %.1f) zoom=%.1f",
                 context.ortho_w / (i32)general.zoom, context.ortho_h / (i32)general.zoom,
                 cori.x(), cori.y(), general.zoom);

        scene.cameras["global_perspective"] =
            std::make_shared<SceneCamera>(
                (float)context.ortho_w / (float)context.ortho_h,
                general.nearz,
                general.farz,
                algorism::CalculatePersperctiveFov(1000.0f, context.ortho_h));

        Vector3f cperori                       = cori;
        cperori[2]                             = 1000.0f;
        context.global_perspective_camera_node = std::make_shared<SceneNode>(cperori, cscale, cangle);
        scene.cameras["global_perspective"]->AttatchNode(context.global_perspective_camera_node);
        scene.sceneGraph->AppendChild(context.global_perspective_camera_node);
    }
}

void InitContext(ParseContext& context, fs::VFS& vfs, wpscene::WPScene& sc) {
    context.scene            = std::make_shared<Scene>();
    context.vfs              = &vfs;
    auto& scene              = *context.scene;
    scene.imageParser        = std::make_unique<WPTexImageParser>(&vfs);
    scene.paritileSys->gener = std::make_unique<WPParticleRawGener>();
    scene.shaderValueUpdater = std::make_unique<WPShaderValueUpdater>(&scene);
    GenCardMesh(scene.default_effect_mesh, { 2, 2 });
    context.shader_updater = static_cast<WPShaderValueUpdater*>(scene.shaderValueUpdater.get());

    scene.clearColor = sc.general.clearcolor;
    scene.ortho[0]   = sc.general.orthogonalprojection.width;
    scene.ortho[1]   = sc.general.orthogonalprojection.height;
    context.ortho_w  = scene.ortho[0];
    context.ortho_h  = scene.ortho[1];
    LOG_INFO("Scene ortho: %dx%d (auto=%d) clearColor=(%.3f,%.3f,%.3f)", context.ortho_w, context.ortho_h,
             sc.general.orthogonalprojection.auto_,
             scene.clearColor[0], scene.clearColor[1], scene.clearColor[2]);

    {
        auto& gb              = context.global_base_uniforms;
        gb["g_ViewUp"]        = std::array { 0.0f, 1.0f, 0.0f };
        gb["g_ViewRight"]     = std::array { 1.0f, 0.0f, 0.0f };
        gb["g_ViewForward"]   = std::array { 0.0f, 0.0f, -1.0f };
        if (! sc.general.isOrtho) {
            gb["g_EyePosition"] = sc.camera.eye;
        } else {
            gb["g_EyePosition"] = std::array { 0.0f, 0.0f, 0.0f };
        }
        gb["g_TexelSize"]     = std::array { 1.0f / 1920.0f, 1.0f / 1080.0f };
        gb["g_TexelSizeHalf"] = std::array { 1.0f / 1920.0f / 2.0f, 1.0f / 1080.0f / 2.0f };

        gb["g_LightAmbientColor"] = sc.general.ambientcolor;
        gb["g_NormalModelMatrix"] = ShaderValue::fromMatrix(Matrix4f::Identity());
    }

    {
        WPCameraParallax cam_para;
        cam_para.enable         = sc.general.cameraparallax;
        cam_para.amount         = sc.general.cameraparallaxamount;
        cam_para.delay          = sc.general.cameraparallaxdelay;
        cam_para.mouseinfluence = sc.general.cameraparallaxmouseinfluence;
        context.shader_updater->SetCameraParallax(cam_para);
    }
    {
        WPCameraShake cam_shake;
        cam_shake.enable    = sc.general.camerashake;
        cam_shake.amplitude = sc.general.camerashakeamplitude;
        cam_shake.speed     = sc.general.camerashakespeed;
        cam_shake.roughness = sc.general.camerashakeroughness;
        context.shader_updater->SetCameraShake(cam_shake);
    }
}

void ParseImageObj(ParseContext& context, wpscene::WPImageObject& img_obj) {
    auto& wpimgobj = img_obj;
    auto& vfs      = *context.vfs;

    LOG_INFO("ParseImageObj: id=%d name='%s' image='%s' visible=%d size=(%.0f,%.0f) fullscreen=%d origin=(%.1f,%.1f)",
             wpimgobj.id, wpimgobj.name.c_str(), wpimgobj.image.c_str(), (int)wpimgobj.visible,
             wpimgobj.size[0], wpimgobj.size[1], (int)wpimgobj.fullscreen,
             wpimgobj.origin[0], wpimgobj.origin[1]);

    // Shape-quad size fallback: if no image and size is still default (2,2),
    // use the scene ortho dimensions so effect pingpong RTs have meaningful resolution.
    if (wpimgobj.image.empty() && wpimgobj.size[0] <= 2.0f && wpimgobj.size[1] <= 2.0f) {
        wpimgobj.size = { (float)context.ortho_w, (float)context.ortho_h };
        LOG_INFO("  shape-quad size fallback: %dx%d", context.ortho_w, context.ortho_h);
    }

    // Invisible nodes are processed as offscreen dependency nodes:
    // their output is written to a per-node RT (not the main scene output) so
    // that compose layers can reference it via _rt_imageLayerComposite_XXX_a → _rt_link_XXX.
    bool isOffscreen = ! wpimgobj.visible;

    // colorBlendMode: prefer hardware blend when a direct equivalent exists;
    // fall back to shader-based effectpassthrough for complex modes.
    // The override is applied after LoadMaterial (material not yet in scope).
    BlendMode colorBlendOverride = BlendMode::Disable; // Disable = no override
    if (wpimgobj.colorBlendMode != 0) {
        switch (wpimgobj.colorBlendMode) {
        case 7: // Screen
            colorBlendOverride = BlendMode::Opaque;
            break;
        case 9: // Add
            colorBlendOverride = BlendMode::Additive;
            break;
        default: {
            wpscene::WPImageEffect colorEffect;
            wpscene::WPMaterial    colorMat;
            nlohmann::json         json;
            if (! PARSE_JSON(
                    fs::GetFileContent(vfs, "/assets/materials/util/effectpassthrough.json"), json))
                return;
            colorMat.FromJson(json);
            colorMat.combos["BONECOUNT"] = 1;
            colorMat.combos["BLENDMODE"] = wpimgobj.colorBlendMode;
            colorMat.blending            = "disabled";
            colorEffect.materials.push_back(colorMat);
            wpimgobj.effects.push_back(colorEffect);
            break;
        }
        }
        LOG_INFO("  colorBlendMode=%d hw_override=%d", wpimgobj.colorBlendMode,
                 colorBlendOverride != BlendMode::Disable);
    }

    // Count effects after colorBlendMode may have added one
    int32_t count_eff = 0;
    for (const auto& wpeffobj : wpimgobj.effects) {
        if (wpeffobj.visible) count_eff++;
    }
    bool hasEffect = count_eff > 0;
    // skip no effect fullscreen layer
    if (! hasEffect && wpimgobj.fullscreen) {
        return;
    }

    bool hasPuppet = ! wpimgobj.puppet.empty();
    (void)hasPuppet;

    bool isCompose = (wpimgobj.image == "models/util/composelayer.json");
    // skip no effect compose layer
    // it's not the correct behaviour, but do it for now
    if (! hasEffect && isCompose) {
        return;
    }

    std::unique_ptr<WPMdl> puppet;
    if (! wpimgobj.puppet.empty()) {
        puppet = std::make_unique<WPMdl>();
        if (! WPMdlParser::Parse(wpimgobj.puppet, vfs, *puppet)) {
            LOG_ERROR("parse puppet failed: %s", wpimgobj.puppet.c_str());
            puppet = nullptr;
        }
        else if (puppet->puppet->bones.size() == 0){
            LOG_ERROR("puppet has no bones: %s", wpimgobj.puppet.c_str());
            puppet = nullptr;
        }
    }

    // wpimgobj.origin[1] = context.ortho_h - wpimgobj.origin[1];
    auto spImgNode = std::make_shared<SceneNode>(Vector3f(wpimgobj.origin.data()),
                                                 Vector3f(wpimgobj.scale.data()),
                                                 Vector3f(wpimgobj.angles.data()));
    LoadAlignment(*spImgNode, wpimgobj.alignment, { wpimgobj.size[0], wpimgobj.size[1] });
    spImgNode->ID() = wpimgobj.id;
    spImgNode->SetOffscreen(isOffscreen);

    SceneMaterial     material;
    WPShaderValueData svData;

    ShaderValueMap baseConstSvs = context.global_base_uniforms;
    WPShaderInfo   shaderInfo;
    {
        if (! hasEffect) {
            svData.parallaxDepth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
            if (puppet) {
                WPMdlParser::AddPuppetShaderInfo(shaderInfo, *puppet);
            }
        }

        baseConstSvs["g_Color4"]     = std::array<float, 4> {
            wpimgobj.color[0],
            wpimgobj.color[1],
            wpimgobj.color[2],
            wpimgobj.alpha
        };
        baseConstSvs["g_UserAlpha"]  = wpimgobj.alpha;
        baseConstSvs["g_Brightness"] = wpimgobj.brightness;

        shaderInfo.baseConstSvs = baseConstSvs;

        if (! LoadMaterial(vfs,
                           wpimgobj.material,
                           context.scene.get(),
                           spImgNode.get(),
                           &material,
                           &svData,
                           &shaderInfo)) {
            LOG_ERROR("load imageobj id=%d '%s' material failed", wpimgobj.id, wpimgobj.name.c_str());
            return;
        };
        LoadConstvalue(material, wpimgobj.material, shaderInfo);

    }

    for (const auto& cs : wpimgobj.material.constantshadervalues) {
        const auto&               name  = cs.first;
        const std::vector<float>& value = cs.second;
        std::string               glname;
        if (shaderInfo.alias.count(name) != 0) {
            glname = shaderInfo.alias.at(name);
        } else {
            for (const auto& el : shaderInfo.alias) {
                if (el.second.substr(2) == name) {
                    glname = el.second;
                    break;
                }
            }
        }
        if (glname.empty()) {
            LOG_ERROR("ShaderValue: %s not found in glsl", name.c_str());
        } else {
            material.customShader.constValues[glname] = value;
        }
    }

    // mesh
    SceneMesh effct_final_mesh {};
    auto      spMesh = std::make_shared<SceneMesh>();
    auto&     mesh   = *spMesh;

    {
        // deal with pow of 2
        std::array<float, 2> mapRate { 1.0f, 1.0f };
        if (! wpimgobj.nopadding &&
            exists(material.customShader.constValues, WE_GLTEX_RESOLUTION_NAMES[0])) {
            const auto& r = material.customShader.constValues.at(WE_GLTEX_RESOLUTION_NAMES[0]);
            mapRate       = { r[2] / r[0], r[3] / r[1] };
        }

        if (puppet) {
            if (hasEffect) {
                GenCardMesh(
                    mesh, { (uint16_t)wpimgobj.size[0], (uint16_t)wpimgobj.size[1] }, mapRate);
                WPMdlParser::GenPuppetMesh(effct_final_mesh, *puppet);

                wpscene::WPImageEffect puppet_effect;
                wpscene::WPMaterial    puppet_mat;
                puppet_mat             = wpimgobj.material;
                puppet_mat.textures[0] = "";
                WPMdlParser::AddPuppetMatInfo(puppet_mat, *puppet);
                puppet_effect.materials.push_back(puppet_mat);
                wpimgobj.effects.push_back(puppet_effect);
            } else {
                svData.puppet_layer = WPPuppetLayer(puppet->puppet);
                svData.puppet_layer.prepared(wpimgobj.puppet_layers);
                WPMdlParser::GenPuppetMesh(mesh, *puppet);
            }
        }
        if (! puppet) {
            GenCardMesh(mesh, { (uint16_t)wpimgobj.size[0], (uint16_t)wpimgobj.size[1] }, mapRate);
            GenCardMesh(effct_final_mesh,
                        { (uint16_t)wpimgobj.size[0], (uint16_t)wpimgobj.size[1] });
        }
    }
    // Apply colorBlendMode hardware override if set
    if (colorBlendOverride != BlendMode::Disable) {
        material.blenmode = colorBlendOverride;
    }
    // material blendmode for last step to use
    auto imgBlendMode = material.blenmode;
    if (hasEffect) {
        LOG_INFO("  ParseImageObj id=%d: finalBlend=%d hasEffect=1 isCompose=%d passthrough=%d",
                 wpimgobj.id, (int)imgBlendMode, isCompose,
                 isCompose && wpimgobj.config.passthrough);
    }
    // disable img material blend, as it's the first effect node now
    if (hasEffect) {
        material.blenmode = BlendMode::Normal;
    }
    mesh.AddMaterial(std::move(material));
    spImgNode->AddMesh(spMesh);

    // Record usershadervalue bindings for runtime user property updates
    // Must be done after AddMaterial since material is moved
    if (! wpimgobj.material.userShaderBindings.empty() && spMesh->Material()) {
        for (auto& [propName, shaderConstName] : wpimgobj.material.userShaderBindings) {
            std::string glname;
            if (shaderInfo.alias.count(shaderConstName) != 0) {
                glname = shaderInfo.alias.at(shaderConstName);
            } else {
                for (const auto& el : shaderInfo.alias) {
                    if (el.second.substr(2) == shaderConstName) {
                        glname = el.second;
                        break;
                    }
                }
            }
            if (glname.empty()) glname = shaderConstName;
            context.scene->userPropUniformBindings[propName].push_back(
                { spMesh->Material(), glname });
            LOG_INFO("  user prop binding: '%s' -> uniform '%s' on id=%d",
                     propName.c_str(), glname.c_str(), wpimgobj.id);
        }
    }

    context.shader_updater->SetNodeData(spImgNode.get(), svData);
    if (hasEffect) {
        auto& scene = *context.scene;
        // currently use addr for unique
        std::string nodeAddr = getAddr(spImgNode.get());
        // set camera to attatch effect
        if (isCompose) {
            scene.cameras[nodeAddr] =
                std::make_shared<SceneCamera>((int32_t)scene.activeCamera->Width(),
                                              (int32_t)scene.activeCamera->Height(),
                                              -1.0f,
                                              1.0f);
            scene.cameras.at(nodeAddr)->AttatchNode(scene.activeCamera->GetAttachedNode());
            if (scene.linkedCameras.count("global") == 0) scene.linkedCameras["global"] = {};
            scene.linkedCameras.at("global").push_back(nodeAddr);
        } else {
            // applly scale to crop
            i32 w                   = (i32)wpimgobj.size[0];
            i32 h                   = (i32)wpimgobj.size[1];
            scene.cameras[nodeAddr] = std::make_shared<SceneCamera>(w, h, -1.0f, 1.0f);
            scene.cameras.at(nodeAddr)->AttatchNode(context.effect_camera_node);
        }
        spImgNode->SetCamera(nodeAddr);
        std::string effect_ppong_a, effect_ppong_b;
        effect_ppong_a = WE_EFFECT_PPONG_PREFIX_A.data() + nodeAddr;
        effect_ppong_b = WE_EFFECT_PPONG_PREFIX_B.data() + nodeAddr;
        // set image effect
        auto imgEffectLayer = std::make_shared<SceneImageEffectLayer>(
            spImgNode.get(), wpimgobj.size[0], wpimgobj.size[1], effect_ppong_a, effect_ppong_b);
        {
            imgEffectLayer->SetFinalBlend(imgBlendMode);
            imgEffectLayer->SetOffscreen(isOffscreen);
            imgEffectLayer->SetPassthrough(isCompose && wpimgobj.config.passthrough);
            // Only visible (non-offscreen) nodes inherit the parent-group
            // transform.  Offscreen dependency nodes render into their own
            // fixed-size RTs and must stay centered — applying the parent
            // group's position would displace their content out of frame.
            imgEffectLayer->SetInheritParent(!isOffscreen &&
                                             wpimgobj.parent_id >= 0 &&
                                             context.node_map.count(wpimgobj.parent_id) > 0);
            imgEffectLayer->FinalMesh().ChangeMeshDataFrom(effct_final_mesh);
            imgEffectLayer->FinalNode().CopyTrans(*spImgNode);
            imgEffectLayer->FinalNode().SetVisibilityOwner(spImgNode.get());
            if (isCompose) {
            } else {
                spImgNode->CopyTrans(SceneNode());
            }
            scene.cameras.at(nodeAddr)->AttatchImgEffect(imgEffectLayer);
        }
        // set renderTarget for ping-pong operate
        {
            scene.renderTargets[effect_ppong_a] = {
                .width      = (uint16_t)wpimgobj.size[0],
                .height     = (uint16_t)wpimgobj.size[1],
                .allowReuse = true,
            };
            if (wpimgobj.fullscreen) {
                scene.renderTargets[effect_ppong_a].bind = { .enable = true, .screen = true };
            }
            scene.renderTargets[effect_ppong_b] = scene.renderTargets.at(effect_ppong_a);
            if (isOffscreen) {
                // Dedicated RT for the final output of invisible dependency nodes.
                scene.renderTargets[GenOffscreenRT(wpimgobj.id)] =
                    scene.renderTargets.at(effect_ppong_a);
            }
        }

        int32_t i_eff = -1;
        for (const auto& wpeffobj : wpimgobj.effects) {
            i_eff++;
            if (! wpeffobj.visible) {
                i_eff--;
                continue;
            }
            std::shared_ptr<SceneImageEffect> imgEffect = std::make_shared<SceneImageEffect>();

            // this will be replace when resolve, use here to get rt info
            const std::string inRT { effect_ppong_a };

            // fbo name map and effect command
            std::string effaddr = getAddr(imgEffectLayer.get());

            std::unordered_map<std::string, std::string> fboMap;
            {
                fboMap["previous"] = inRT;
                for (usize i = 0; i < wpeffobj.fbos.size(); i++) {
                    const auto& wpfbo  = wpeffobj.fbos.at(i);
                    std::string rtname = std::string(WE_SPEC_PREFIX) + wpfbo.name + "_" + effaddr;
                    if (wpimgobj.fullscreen) {
                        scene.renderTargets[rtname]      = { 2, 2, true };
                        scene.renderTargets[rtname].bind = {
                            .enable = true,
                            .screen = true,
                            .scale  = 1.0 / wpfbo.scale,
                        };
                    } else {
                        // i+2 for not override object's rt
                        scene.renderTargets[rtname] = {
                            .width      = (uint16_t)(wpimgobj.size[0] / (float)wpfbo.scale),
                            .height     = (uint16_t)(wpimgobj.size[1] / (float)wpfbo.scale),
                            .allowReuse = true
                        };
                    }
                    fboMap[wpfbo.name] = rtname;
                }
            }
            // load! effect commands
            {
                for (const auto& el : wpeffobj.commands) {
                    SceneImageEffect::CmdType cmdType;
                    if (el.command == "copy") {
                        cmdType = SceneImageEffect::CmdType::Copy;
                    } else if (el.command == "swap") {
                        cmdType = SceneImageEffect::CmdType::Swap;
                    } else {
                        LOG_ERROR("Unknown effect command: %s", el.command.c_str());
                        continue;
                    }
                    if (fboMap.count(el.target) + fboMap.count(el.source) < 2) {
                        LOG_ERROR("Unknown effect command dst or src: %s %s",
                                  el.target.c_str(),
                                  el.source.c_str());
                        continue;
                    }
                    imgEffect->commands.push_back({ .cmd      = cmdType,
                                                    .dst      = fboMap[el.target],
                                                    .src      = fboMap[el.source],
                                                    .afterpos = el.afterpos });
                }
            }

            bool eff_mat_ok { true };

            for (usize i_mat = 0; i_mat < wpeffobj.materials.size(); i_mat++) {
                wpscene::WPMaterial wpmat = wpeffobj.materials.at(i_mat);
                std::string         matOutRT { WE_EFFECT_PPONG_PREFIX_B };
                if (wpeffobj.passes.size() > i_mat) {
                    const auto& wppass = wpeffobj.passes.at(i_mat);
                    wpmat.MergePass(wppass);
                    // Set rendertarget, in and out
                    for (const auto& el : wppass.bind) {
                        if (fboMap.count(el.name) == 0) {
                            LOG_ERROR("fbo %s not found", el.name.c_str());
                            continue;
                        }
                        if (wpmat.textures.size() <= (usize)el.index)
                            wpmat.textures.resize((usize)el.index + 1);
                        wpmat.textures[(usize)el.index] = fboMap[el.name];
                    }
                    if (! wppass.target.empty()) {
                        if (fboMap.count(wppass.target) == 0) {
                            LOG_ERROR("fbo %s not found", wppass.target.c_str());
                        } else {
                            matOutRT = fboMap.at(wppass.target);
                        }
                    }
                }
                if (wpmat.textures.size() == 0) wpmat.textures.resize(1);
                if (wpmat.textures.at(0).empty()) {
                    wpmat.textures[0] = inRT;
                }
                auto         spEffNode  = std::make_shared<SceneNode>();
                std::string  effmataddr = getAddr(spEffNode.get());
                WPShaderInfo wpEffShaderInfo;
                wpEffShaderInfo.baseConstSvs = baseConstSvs;
                wpEffShaderInfo.baseConstSvs["g_EffectTextureProjectionMatrix"] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                wpEffShaderInfo.baseConstSvs["g_EffectTextureProjectionMatrixInverse"] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                SceneMaterial     material;
                WPShaderValueData svData;
                if (! LoadMaterial(vfs,
                                   wpmat,
                                   context.scene.get(),
                                   spEffNode.get(),
                                   &material,
                                   &svData,
                                   &wpEffShaderInfo)) {
                    eff_mat_ok = false;
                    break;
                }

                // load glname from alias and load to constvalue
                LoadConstvalue(material, wpmat, wpEffShaderInfo);
                auto spMesh = std::make_shared<SceneMesh>();
                {
                    svData.parallaxDepth = { wpimgobj.parallaxDepth[0], wpimgobj.parallaxDepth[1] };
                    if (puppet && wpmat.use_puppet) {
                        svData.puppet_layer = WPPuppetLayer(puppet->puppet);
                        svData.puppet_layer.prepared(wpimgobj.puppet_layers);
                    }
                }
                spMesh->AddMaterial(std::move(material));
                spEffNode->AddMesh(spMesh);

                context.shader_updater->SetNodeData(spEffNode.get(), svData);
                spEffNode->SetVisibilityOwner(spImgNode.get());
                imgEffect->nodes.push_back({ matOutRT, spEffNode });
            }

            if (eff_mat_ok) {
                imgEffectLayer->AddEffect(imgEffect);
                LOG_INFO("  effect[%d] '%s' loaded OK (%zu nodes)", i_eff,
                         wpeffobj.name.c_str(), imgEffect->nodes.size());
            }
            else {
                LOG_ERROR("effect \'%s\' failed to load", wpeffobj.name.c_str());
            }
        }
        LOG_INFO("  ParseImageObj id=%d: %zu effects loaded, isCompose=%d, isOffscreen=%d",
                 wpimgobj.id, imgEffectLayer->EffectCount(), (int)isCompose, (int)isOffscreen);
    }
    // Invisible nodes without effects still need an offscreen RT so their output
    // can be referenced via link tex by compose layers.
    if (isOffscreen && ! hasEffect) {
        auto& scene = *context.scene;
        scene.renderTargets[GenOffscreenRT(wpimgobj.id)] = {
            .width      = (uint16_t)wpimgobj.size[0],
            .height     = (uint16_t)wpimgobj.size[1],
            .allowReuse = true,
        };
        LOG_INFO("  created offscreen RT '%s' for id=%d (%dx%d)",
                 GenOffscreenRT(wpimgobj.id).c_str(), wpimgobj.id,
                 (int)wpimgobj.size[0], (int)wpimgobj.size[1]);
    }
    // Add to parent node if this object has a parent, otherwise to root scene graph
    if (wpimgobj.parent_id >= 0 && context.node_map.count(wpimgobj.parent_id)) {
        context.node_map.at(wpimgobj.parent_id)->AppendChild(spImgNode);
        // Offscreen nodes render into dedicated RTs with their own camera;
        // clear parent so UpdateTrans() doesn't chain the group transform
        // (the node stays in the parent's children list for traversal).
        if (isOffscreen) {
            spImgNode->InheritParent(SceneNode());
        }
        LOG_INFO("  ParseImageObj id=%d completed, added as child of parent %d (parent_cleared=%d)",
                 wpimgobj.id, wpimgobj.parent_id, (int)isOffscreen);
    } else {
        context.scene->sceneGraph->AppendChild(spImgNode);
        LOG_INFO("  ParseImageObj id=%d completed, added to scene graph", wpimgobj.id);
    }
    // Register this node in the map so other objects can reference it as parent
    context.node_map[wpimgobj.id] = spImgNode;
}

struct ParticleChildPtr {
    wpscene::ParticleChild* child { nullptr };
    SceneNode*              node_parent { nullptr };
    ParticleSubSystem*      particle_parent { nullptr };

    i32 max_instancecount { 1 };
};

void ParseParticleObj(ParseContext& context, wpscene::WPParticleObject& wppartobj,
                      ParticleChildPtr child_ptr = {}) {
    struct ChildData {
        ChildData() = default;
        ChildData(const wpscene::ParticleChild& o)
            : type(o.type),
              maxcount(o.maxcount),
              controlpointstartindex(o.controlpointstartindex),
              probability(o.probability) {}
        std::string type { "static" };
        i32         maxcount { 20 };
        i32         controlpointstartindex { 0 };
        float       probability { 1.0f };
    };

    wpscene::Particle*         p_particle_obj { nullptr };
    std::shared_ptr<SceneNode> spNode;
    ChildData                  child_data;

    bool is_child = child_ptr.child != nullptr;
    if (is_child) {
        p_particle_obj = &(child_ptr.child->obj);
        spNode         = std::make_shared<SceneNode>(Vector3f(child_ptr.child->origin.data()),
                                             Vector3f(child_ptr.child->scale.data()),
                                             Vector3f(child_ptr.child->angles.data()));
        child_data     = ChildData(*child_ptr.child);

        child_ptr.max_instancecount *= child_data.maxcount;

    } else {
        p_particle_obj = &wppartobj.particleObj;
        spNode         = std::make_shared<SceneNode>(Vector3f(wppartobj.origin.data()),
                                             Vector3f(wppartobj.scale.data()),
                                             Vector3f(wppartobj.angles.data()));
        spNode->ID() = wppartobj.id;
    }

    wpscene::ParticleInstanceoverride override = wppartobj.instanceoverride;

    auto& particle_obj = *p_particle_obj;
    auto& vfs          = *context.vfs;

    if (particle_obj.renderers.empty()) {
        LOG_ERROR("particle '%s' has no renderers, skipping", wppartobj.name.c_str());
        return;
    }
    auto wppartRenderer = particle_obj.renderers.at(0);
    bool render_rope    = sstart_with(wppartRenderer.name, "rope");
    bool hastrail       = send_with(wppartRenderer.name, "trail");

    if (render_rope) particle_obj.material.shader = "genericropeparticle";

    // wppartobj.origin[1] = context.ortho_h - wppartobj.origin[1];

    if (particle_obj.flags[wpscene::Particle::FlagEnum::perspective]) {
        spNode->SetCamera("global_perspective");
    }

    SceneMaterial     material;
    WPShaderValueData svData;

    if (! is_child) {
        svData.parallaxDepth = { wppartobj.parallaxDepth[0], wppartobj.parallaxDepth[1] };
    }

    WPShaderInfo shaderInfo;
    shaderInfo.baseConstSvs                         = context.global_base_uniforms;
    shaderInfo.baseConstSvs["g_OrientationUp"]      = std::array { 0.0f, 1.0f, 0.0f };
    shaderInfo.baseConstSvs["g_OrientationRight"]   = std::array { 1.0f, 0.0f, 0.0f };
    shaderInfo.baseConstSvs["g_OrientationForward"] = std::array { 0.0f, 0.0f, 1.0f };
    shaderInfo.baseConstSvs["g_ViewUp"]             = std::array { 0.0f, 1.0f, 0.0f };
    shaderInfo.baseConstSvs["g_ViewRight"]          = std::array { 1.0f, 0.0f, 0.0f };

    u32 maxcount = particle_obj.maxcount;
    maxcount     = std::min(maxcount, 20000u);

    if (hastrail) {
        double in_SegmentUVTimeOffset           = 0.0;
        double in_SegmentMaxCount               = maxcount - 1.0;
        shaderInfo.baseConstSvs["g_RenderVar0"] = std::array {
            (float)wppartRenderer.length,
            (float)wppartRenderer.maxlength,
            (float)in_SegmentUVTimeOffset,
            (float)in_SegmentMaxCount,
        };
        shaderInfo.combos["THICKFORMAT"]   = "1";
        shaderInfo.combos["TRAILRENDERER"] = "1";
    }

    if (! particle_obj.flags[wpscene::Particle::FlagEnum::spritenoframeblending]) {
        shaderInfo.combos["SPRITESHEETBLEND"] = "1";
    }

    if (! LoadMaterial(vfs,
                       particle_obj.material,
                       context.scene.get(),
                       spNode.get(),
                       &material,
                       &svData,
                       &shaderInfo)) {
        LOG_ERROR("load particleobj '%s' material faild", wppartobj.name.c_str());
        return;
    }
    LoadConstvalue(material, particle_obj.material, shaderInfo);
    auto  spMesh             = std::make_shared<SceneMesh>(true);
    auto& mesh               = *spMesh;
    auto  animationmode      = ToAnimMode(particle_obj.animationmode);
    auto  sequencemultiplier = particle_obj.sequencemultiplier;
    bool  hasSprite          = material.hasSprite;
    (void)hasSprite;

    bool thick_format = material.hasSprite || hastrail;
    {
        u32 mesh_maxcount = maxcount * (u32)child_ptr.max_instancecount;
        if (render_rope)
            SetRopeParticleMesh(mesh, particle_obj, mesh_maxcount, thick_format);
        else
            SetParticleMesh(mesh, particle_obj, mesh_maxcount, thick_format);
    }

    auto particleSub = std::make_unique<ParticleSubSystem>(
        *context.scene->paritileSys,
        spMesh,
        maxcount,
        override.rate,
        child_data.maxcount,
        child_data.probability,
        ParseSpawnType(child_data.type),
        [=](const Particle& p, const ParticleRawGenSpec& spec) {
            auto& lifetime = *(spec.lifetime);
            if (lifetime <= 0.0f) {
                lifetime = 0.0f;
                return;
            }
            switch (animationmode) {
            case ParticleAnimationMode::RANDOMONE: lifetime = std::floor(p.init.lifetime); break;
            case ParticleAnimationMode::SEQUENCE:
                lifetime = (1.0f - (p.lifetime / p.init.lifetime)) * sequencemultiplier;
                break;
            }
        });

    LoadEmitter(*particleSub, particle_obj, override.count, render_rope);
    LoadInitializer(*particleSub, particle_obj, override);
    LoadOperator(*particleSub, particle_obj, override);
    LoadControlPoint(*particleSub, particle_obj);

    mesh.AddMaterial(std::move(material));
    spNode->AddMesh(spMesh);
    context.shader_updater->SetNodeData(spNode.get(), svData);

    for (auto& child : particle_obj.children) {
        ParseParticleObj(context,
                         wppartobj,
                         {
                             .child             = &child,
                             .node_parent       = spNode.get(),
                             .particle_parent   = particleSub.get(),
                             .max_instancecount = child_ptr.max_instancecount,
                         });
    }

    if (is_child)
        child_ptr.particle_parent->AddChild(std::move(particleSub));
    else
        context.scene->paritileSys->subsystems.emplace_back(std::move(particleSub));

    if (is_child) {
        child_ptr.node_parent->AppendChild(spNode);
    } else if (wppartobj.parent_id >= 0 && context.node_map.count(wppartobj.parent_id)) {
        context.node_map.at(wppartobj.parent_id)->AppendChild(spNode);
    } else {
        context.scene->sceneGraph->AppendChild(spNode);
    }
    context.node_map[wppartobj.id] = spNode;
}

void ParseLightObj(ParseContext& context, wpscene::WPLightObject& light_obj) {
    auto node = std::make_shared<SceneNode>(Vector3f(light_obj.origin.data()),
                                            Vector3f(light_obj.scale.data()),
                                            Vector3f(light_obj.angles.data()));

    context.scene->lights.emplace_back(std::make_unique<SceneLight>(
        Vector3f(light_obj.color.data()), light_obj.radius, light_obj.intensity));

    auto& light = *(context.scene->lights.back());
    light.setNode(node);

    context.scene->sceneGraph->AppendChild(node);
}

void ParseModelObj(ParseContext& context, wpscene::WPModelObject& model_obj) {
    auto& vfs = *context.vfs;

    LOG_INFO("ParseModelObj: id=%d name='%s' model='%s'",
             model_obj.id, model_obj.name.c_str(), model_obj.model.c_str());

    WPMdl mdl;
    if (! WPMdlParser::Parse(model_obj.model, vfs, mdl)) {
        LOG_ERROR("parse model failed: %s", model_obj.model.c_str());
        return;
    }

    // Parent node holds the transform; submesh nodes are children
    auto spParent = std::make_shared<SceneNode>(Vector3f(model_obj.origin.data()),
                                                Vector3f(model_obj.scale.data()),
                                                Vector3f(model_obj.angles.data()));
    spParent->ID() = model_obj.id;

    std::vector<std::shared_ptr<SceneNode>> translucentNodes;
    for (size_t si = 0; si < mdl.submeshes.size(); si++) {
        auto& sub = mdl.submeshes[si];

        // Load material JSON from the submesh's referenced material file
        nlohmann::json jMat;
        if (! PARSE_JSON(fs::GetFileContent(vfs, "/assets/" + sub.mat_json_file), jMat)) {
            LOG_ERROR("Can't load model material json: %s", sub.mat_json_file.c_str());
            continue;
        }
        wpscene::WPMaterial wpmat;
        wpmat.FromJson(jMat);

        // 3D models need depth enabled (WPMaterial defaults are 2D: disabled)
        wpmat.depthtest  = "enabled";
        wpmat.depthwrite = "enabled";

        // Materials that need transparency explicitly set "blending": "translucent".
        // Without it, default to opaque for 3D models (WPMaterial default is translucent for 2D).
        const auto& jContent = jMat.at("passes").at(0);
        if (! jContent.contains("blending")) {
            wpmat.blending = "normal";
        }
        // Translucent materials: keep depth test but disable depth write so
        // objects behind transparent surfaces remain visible.
        if (wpmat.blending == "translucent") {
            wpmat.depthwrite = "disabled";
        }
        // Default 3D models to backface culling when not specified.
        // Translucent flat quads (grid, shadow) may face either way,
        // so skip culling for translucent materials.
        if (! jContent.contains("cullmode")) {
            wpmat.cullmode = (wpmat.blending == "translucent") ? "nocull" : "back";
        }

        // Submesh child node (identity transform — parent holds the model transform)
        auto spNode = std::make_shared<SceneNode>();
        // First submesh gets the model object ID; others get synthetic IDs
        spNode->ID() = (si == 0) ? model_obj.id : -(model_obj.id * 100 + (i32)si);

        SceneMaterial     material;
        WPShaderValueData svData;
        WPShaderInfo      shaderInfo;
        shaderInfo.baseConstSvs = context.global_base_uniforms;

        if (! LoadMaterial(vfs,
                           wpmat,
                           context.scene.get(),
                           spNode.get(),
                           &material,
                           &svData,
                           &shaderInfo)) {
            LOG_ERROR("load model id=%d submesh %zu '%s' material failed",
                      model_obj.id, si, sub.mat_json_file.c_str());
            continue;
        }
        LOG_INFO("  submesh[%zu] shader '%s' mat='%s' verts=%zu tris=%zu blend=%s",
                 si, wpmat.shader.c_str(), sub.mat_json_file.c_str(),
                 sub.vertexs.size(), sub.indices.size(), wpmat.blending.c_str());
        LoadConstvalue(material, wpmat, shaderInfo);

        // Single-submesh translucent models (grid, shadow) are typically
        // floor-level objects coplanar with the dome base. Negative depth bias
        // pulls them closer to the camera so they pass depth test against the dome.
        if (mdl.submeshes.size() == 1 && wpmat.blending == "translucent") {
            material.depthBiasConstant = -5000.0f;
        }

        auto  spMesh = std::make_shared<SceneMesh>();
        auto& mesh   = *spMesh;
        WPMdlParser::GenModelMesh(mesh, sub);

        mesh.AddMaterial(std::move(material));
        spNode->AddMesh(spMesh);
        context.shader_updater->SetNodeData(spNode.get(), svData);

        // Defer translucent submeshes so opaque geometry renders first
        bool isTranslucent = (wpmat.blending == "translucent");
        if (isTranslucent) {
            translucentNodes.push_back(spNode);
        } else {
            spParent->AppendChild(spNode);
        }
    }
    // Append translucent submeshes after all opaque ones
    for (auto& tn : translucentNodes) {
        spParent->AppendChild(tn);
    }

    if (model_obj.parent_id >= 0 && context.node_map.count(model_obj.parent_id)) {
        context.node_map.at(model_obj.parent_id)->AppendChild(spParent);
    } else {
        context.scene->sceneGraph->AppendChild(spParent);
    }
    context.node_map[model_obj.id] = spParent;
    LOG_INFO("  ParseModelObj id=%d completed, %zu submeshes",
             model_obj.id, mdl.submeshes.size());
}

void ParseTextObj(ParseContext& context, wpscene::WPTextObject& textObj) {
    auto& vfs = *context.vfs;

    LOG_INFO("ParseTextObj: id=%d name='%s' font='%s' text='%s' visible=%d",
             textObj.id, textObj.name.c_str(), textObj.font.c_str(), textObj.textValue.c_str(),
             (int)textObj.visible);

    if (!textObj.visible) {
        LOG_INFO("  text object is invisible, skipping");
        return;
    }

    // Skip empty text or system fonts we can't load
    if (textObj.textValue.empty()) {
        LOG_INFO("  text is empty, skipping");
        return;
    }
    if (textObj.font.empty() || textObj.font.find("systemfont") != std::string::npos) {
        LOG_INFO("  system font or missing font '%s', skipping", textObj.font.c_str());
        return;
    }

    // Load font from VFS — try /assets/ prefix first (PKG assets), then bare path
    std::string fontData;
    if (vfs.Contains("/assets/" + textObj.font)) {
        fontData = fs::GetFileContent(vfs, "/assets/" + textObj.font);
    } else if (vfs.Contains("/" + textObj.font)) {
        fontData = fs::GetFileContent(vfs, "/" + textObj.font);
    }
    if (fontData.empty()) {
        LOG_ERROR("  failed to load font: %s", textObj.font.c_str());
        return;
    }

    i32 texW = static_cast<i32>(textObj.size[0]);
    i32 texH = static_cast<i32>(textObj.size[1]);
    if (texW <= 0 || texH <= 0) {
        texW = 512;
        texH = 128;
    }

    // Rasterize text
    auto textImage = WPTextRenderer::RenderText(fontData, textObj.pointsize, textObj.textValue,
                                                 texW, texH, textObj.horizontalalign,
                                                 textObj.verticalalign, textObj.padding);
    if (!textImage) {
        LOG_ERROR("  text rasterization failed");
        return;
    }

    // Register texture with a unique key
    std::string texKey = "_text_" + std::to_string(textObj.id);
    textImage->key     = texKey;

    auto* imgParser = dynamic_cast<WPTexImageParser*>(context.scene->imageParser.get());
    if (imgParser) {
        imgParser->RegisterImage(texKey, textImage);
    }

    // Register in scene textures
    SceneTexture stex;
    stex.url    = texKey;
    stex.sample = { TextureWrap::CLAMP_TO_EDGE, TextureWrap::CLAMP_TO_EDGE,
                    TextureFilter::LINEAR, TextureFilter::LINEAR };
    context.scene->textures[texKey] = stex;

    // Build material: genericimage2 shader with text texture
    wpscene::WPMaterial wpMat;
    wpMat.shader   = "genericimage2";
    wpMat.blending = "translucent";
    wpMat.textures.push_back(texKey);

    // Create scene node
    auto spNode = std::make_shared<SceneNode>(Vector3f(textObj.origin.data()),
                                               Vector3f(textObj.scale.data()),
                                               Vector3f(textObj.angles.data()));
    spNode->ID() = textObj.id;

    // Load material
    SceneMaterial     material;
    WPShaderValueData svData;
    WPShaderInfo      shaderInfo;
    shaderInfo.baseConstSvs = context.global_base_uniforms;
    shaderInfo.baseConstSvs["g_Color4"] = std::array<float, 4> {
        textObj.color[0], textObj.color[1], textObj.color[2], textObj.alpha
    };
    shaderInfo.baseConstSvs["g_UserAlpha"]  = textObj.alpha;
    shaderInfo.baseConstSvs["g_Brightness"] = textObj.brightness;

    if (!LoadMaterial(vfs, wpMat, context.scene.get(), spNode.get(),
                      &material, &svData, &shaderInfo)) {
        LOG_ERROR("  load text material failed");
        return;
    }
    LoadConstvalue(material, wpMat, shaderInfo);

    // Count effects
    int32_t count_eff = 0;
    for (const auto& eff : textObj.effects) {
        if (eff.visible) count_eff++;
    }
    bool hasEffect = count_eff > 0;

    // Disable base material blend if it will be the first effect node
    auto imgBlendMode = material.blenmode;
    if (hasEffect) {
        material.blenmode = BlendMode::Normal;
    }

    // Mesh
    auto spMesh = std::make_shared<SceneMesh>();
    GenCardMesh(*spMesh, { static_cast<uint16_t>(texW), static_cast<uint16_t>(texH) });
    spMesh->AddMaterial(std::move(material));
    spNode->AddMesh(spMesh);

    context.shader_updater->SetNodeData(spNode.get(), svData);

    if (hasEffect) {
        auto& scene     = *context.scene;
        std::string nodeAddr = getAddr(spNode.get());

        i32 w = texW;
        i32 h = texH;
        scene.cameras[nodeAddr] = std::make_shared<SceneCamera>(w, h, -1.0f, 1.0f);
        scene.cameras.at(nodeAddr)->AttatchNode(context.effect_camera_node);
        spNode->SetCamera(nodeAddr);

        std::string effect_ppong_a = WE_EFFECT_PPONG_PREFIX_A.data() + nodeAddr;
        std::string effect_ppong_b = WE_EFFECT_PPONG_PREFIX_B.data() + nodeAddr;

        SceneMesh effctFinalMesh {};
        GenCardMesh(effctFinalMesh,
                    { static_cast<uint16_t>(w), static_cast<uint16_t>(h) });

        auto imgEffectLayer = std::make_shared<SceneImageEffectLayer>(
            spNode.get(), static_cast<float>(w), static_cast<float>(h),
            effect_ppong_a, effect_ppong_b);
        {
            imgEffectLayer->SetFinalBlend(imgBlendMode);
            imgEffectLayer->FinalMesh().ChangeMeshDataFrom(effctFinalMesh);
            imgEffectLayer->FinalNode().CopyTrans(*spNode);
            imgEffectLayer->FinalNode().SetVisibilityOwner(spNode.get());
            spNode->CopyTrans(SceneNode());
            scene.cameras.at(nodeAddr)->AttatchImgEffect(imgEffectLayer);
        }
        scene.renderTargets[effect_ppong_a] = {
            .width      = static_cast<uint16_t>(w),
            .height     = static_cast<uint16_t>(h),
            .allowReuse = true,
        };
        scene.renderTargets[effect_ppong_b] = scene.renderTargets.at(effect_ppong_a);

        ShaderValueMap baseConstSvs = shaderInfo.baseConstSvs;

        int32_t i_eff = -1;
        for (const auto& wpeffobj : textObj.effects) {
            i_eff++;
            if (!wpeffobj.visible) { i_eff--; continue; }

            auto imgEffect = std::make_shared<SceneImageEffect>();
            const std::string inRT { effect_ppong_a };

            std::string effaddr = getAddr(imgEffectLayer.get());
            std::unordered_map<std::string, std::string> fboMap;
            fboMap["previous"] = inRT;
            for (usize i = 0; i < wpeffobj.fbos.size(); i++) {
                const auto& wpfbo  = wpeffobj.fbos.at(i);
                std::string rtname = wpfbo.name + "_" + effaddr;
                scene.renderTargets[rtname] = {
                    .width      = static_cast<uint16_t>(w / static_cast<float>(wpfbo.scale)),
                    .height     = static_cast<uint16_t>(h / static_cast<float>(wpfbo.scale)),
                    .allowReuse = true
                };
                fboMap[wpfbo.name] = rtname;
            }

            bool eff_mat_ok = true;
            for (usize i_mat = 0; i_mat < wpeffobj.materials.size(); i_mat++) {
                wpscene::WPMaterial wpmat = wpeffobj.materials.at(i_mat);
                std::string matOutRT { WE_EFFECT_PPONG_PREFIX_B };
                if (wpeffobj.passes.size() > i_mat) {
                    const auto& wppass = wpeffobj.passes.at(i_mat);
                    wpmat.MergePass(wppass);
                    for (const auto& el : wppass.bind) {
                        if (fboMap.count(el.name) == 0) continue;
                        if (wpmat.textures.size() <= static_cast<usize>(el.index))
                            wpmat.textures.resize(static_cast<usize>(el.index) + 1);
                        wpmat.textures[static_cast<usize>(el.index)] = fboMap[el.name];
                    }
                    if (!wppass.target.empty() && fboMap.count(wppass.target))
                        matOutRT = fboMap.at(wppass.target);
                }
                if (wpmat.textures.empty()) wpmat.textures.resize(1);
                if (wpmat.textures.at(0).empty()) wpmat.textures[0] = inRT;

                auto spEffNode = std::make_shared<SceneNode>();
                WPShaderInfo wpEffShaderInfo;
                wpEffShaderInfo.baseConstSvs = baseConstSvs;
                wpEffShaderInfo.baseConstSvs["g_EffectTextureProjectionMatrix"] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                wpEffShaderInfo.baseConstSvs["g_EffectTextureProjectionMatrixInverse"] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());

                SceneMaterial     effMaterial;
                WPShaderValueData effSvData;
                if (!LoadMaterial(vfs, wpmat, context.scene.get(), spEffNode.get(),
                                  &effMaterial, &effSvData, &wpEffShaderInfo)) {
                    eff_mat_ok = false;
                    break;
                }
                LoadConstvalue(effMaterial, wpmat, wpEffShaderInfo);
                auto spEffMesh = std::make_shared<SceneMesh>();
                spEffMesh->AddMaterial(std::move(effMaterial));
                spEffNode->AddMesh(spEffMesh);
                context.shader_updater->SetNodeData(spEffNode.get(), effSvData);
                spEffNode->SetVisibilityOwner(spNode.get());
                imgEffect->nodes.push_back({ matOutRT, spEffNode });
            }
            if (eff_mat_ok) imgEffectLayer->AddEffect(imgEffect);
        }
    }

    // Register text layer info for dynamic script evaluation (Phase 2)
    if (!textObj.textScript.empty()) {
        TextLayerInfo tli;
        tli.id          = textObj.id;
        tli.fontData    = fontData;
        tli.pointsize   = textObj.pointsize;
        tli.texWidth    = texW;
        tli.texHeight   = texH;
        tli.padding     = textObj.padding;
        tli.halign      = textObj.horizontalalign;
        tli.valign      = textObj.verticalalign;
        tli.currentText = textObj.textValue;
        tli.textureKey  = texKey;
        tli.script      = textObj.textScript;
        context.scene->textLayers.push_back(std::move(tli));
        LOG_INFO("  registered text layer id=%d for script evaluation", textObj.id);
    }

    // Add to parent or root
    if (textObj.parent_id >= 0 && context.node_map.count(textObj.parent_id)) {
        context.node_map.at(textObj.parent_id)->AppendChild(spNode);
    } else {
        context.scene->sceneGraph->AppendChild(spNode);
    }
    context.node_map[textObj.id] = spNode;

    LOG_INFO("  ParseTextObj id=%d completed", textObj.id);
}

template<typename T>
void AddWPObject(std::vector<WPObjectVar>& objs, const nlohmann::json& json_obj, fs::VFS& vfs) {
    T wpobj;
    if (! wpobj.FromJson(json_obj, vfs)) {
        LOG_ERROR("parse scene object failed, name: %s", wpobj.name.c_str());
        return;
    }
    objs.push_back(wpobj);
}
} // namespace

std::shared_ptr<Scene> WPSceneParser::Parse(std::string_view scene_id, const std::string& buf,
                                            fs::VFS& vfs, audio::SoundManager& sm,
                                            const std::string& userPropsOverride) {
    // Load user properties from project.json if available
    WPUserProperties userProps;
    if (vfs.Contains("/assets/project.json")) {
        auto projectFile = vfs.Open("/assets/project.json");
        if (projectFile) {
            std::string projectContent = projectFile->ReadAllStr();
            if (userProps.LoadFromProjectJson(projectContent)) {
                LOG_INFO("Loaded %s user properties", userProps.Empty() ? "no" : "some");
            }
        }
    }

    // Apply user overrides if provided
    if (!userPropsOverride.empty()) {
        LOG_INFO("Applying user properties override: %s", userPropsOverride.c_str());
        userProps.ApplyOverrides(userPropsOverride);
    }

    // Set user properties context for the duration of parsing
    UserPropertiesScope propsScope(&userProps);

    nlohmann::json json;
    if (! PARSE_JSON(buf, json)) return nullptr;
    wpscene::WPScene sc;
    sc.FromJson(json);
    //	LOG_INFO(nlohmann::json(sc).dump(4));

    ParseContext context;

    std::vector<WPObjectVar> wp_objs;

    // First pass: create group nodes (objects without image/particle/sound/light).
    // These are structural containers that provide transforms for child objects.
    struct GroupInfo { i32 id; i32 parent_id; };
    std::vector<GroupInfo> group_infos;

    // Track each object's position in the JSON array so we can restore
    // the intended Z-order after the two-pass group/image construction.
    std::map<i32, size_t> json_order;
    size_t                obj_idx = 0;

    for (auto& obj : json.at("objects")) {
        if (obj.contains("id") && obj.at("id").is_number_integer()) {
            json_order[obj.at("id").get<i32>()] = obj_idx;
        }
        obj_idx++;

        if (obj.contains("image") && ! obj.at("image").is_null()) {
            AddWPObject<wpscene::WPImageObject>(wp_objs, obj, vfs);
        } else if (obj.contains("shape") && obj.value("shape", "") == "quad") {
            AddWPObject<wpscene::WPImageObject>(wp_objs, obj, vfs);
        } else if (obj.contains("particle") && ! obj.at("particle").is_null()) {
            AddWPObject<wpscene::WPParticleObject>(wp_objs, obj, vfs);
        } else if (obj.contains("sound") && ! obj.at("sound").is_null()) {
            AddWPObject<wpscene::WPSoundObject>(wp_objs, obj, vfs);
        } else if (obj.contains("light") && ! obj.at("light").is_null()) {
            AddWPObject<wpscene::WPLightObject>(wp_objs, obj, vfs);
        } else if (obj.contains("model") && ! obj.at("model").is_null()) {
            AddWPObject<wpscene::WPModelObject>(wp_objs, obj, vfs);
        } else if (obj.contains("text") && ! obj.at("text").is_null()) {
            AddWPObject<wpscene::WPTextObject>(wp_objs, obj, vfs);
        } else if (obj.contains("id")) {
            // Group node — no content, just a transform container
            try {
                i32 gid = 0;
                GET_JSON_NAME_VALUE(obj, "id", gid);

                std::array<float, 3> origin { 0, 0, 0 };
                std::array<float, 3> scale { 1, 1, 1 };
                std::array<float, 3> angles { 0, 0, 0 };
                GET_JSON_NAME_VALUE_NOWARN(obj, "origin", origin);
                GET_JSON_NAME_VALUE_NOWARN(obj, "scale", scale);
                GET_JSON_NAME_VALUE_NOWARN(obj, "angles", angles);

                auto node = std::make_shared<SceneNode>(
                    Vector3f(origin.data()), Vector3f(scale.data()), Vector3f(angles.data()));
                node->ID() = gid;

                context.node_map[gid] = node;

                i32 parent_id = -1;
                GET_JSON_NAME_VALUE_NOWARN(obj, "parent", parent_id);
                group_infos.push_back({ gid, parent_id });

                LOG_INFO("created group node id=%d origin=(%.1f, %.1f, %.1f) scale=(%.1f, %.1f, %.1f)",
                         gid, origin[0], origin[1], origin[2], scale[0], scale[1], scale[2]);
            } catch (const std::exception& e) {
                LOG_ERROR("failed to parse group node: %s", e.what());
            }
        }
    }

    if (sc.general.orthogonalprojection.auto_) {
        i32 w = 0, h = 0;
        for (auto& obj : wp_objs) {
            auto* img = std::get_if<wpscene::WPImageObject>(&obj);
            if (img == nullptr) continue;
            i32 size = (i32)(img->size.at(0) * img->size.at(1));
            if (size > w * h) {
                w = (i32)img->size.at(0);
                h = (i32)img->size.at(1);
            }
        }
        sc.general.orthogonalprojection.width  = w;
        sc.general.orthogonalprojection.height = h;
    }

    InitContext(context, vfs, sc);
    ParseCamera(context, sc);

    // Build group node hierarchy: add each group to its parent (or scene root)
    for (auto& gi : group_infos) {
        auto& node = context.node_map.at(gi.id);
        if (gi.parent_id >= 0 && context.node_map.count(gi.parent_id)) {
            context.node_map.at(gi.parent_id)->AppendChild(node);
        } else {
            context.scene->sceneGraph->AppendChild(node);
        }
    }

    {
        context.scene->renderTargets[SpecTex_Default.data()] = {
            .width  = context.ortho_w,
            .height = context.ortho_h,
            .bind   = { .enable = true, .screen = true },
        };
        context.scene->renderTargets[WE_MIP_MAPPED_FRAME_BUFFER.data()] = {
            .width      = context.ortho_w,
            .height     = context.ortho_h,
            .has_mipmap = true,
            .bind       = { .enable = true, .name = SpecTex_Default.data() }
        };
        context.scene->renderTargets[WE_SHADOW_ATLAS.data()] = {
            .width  = 2048,
            .height = 2048,
        };
        context.scene->renderTargets[WE_REFLECTION.data()] = {
            .width  = context.ortho_w,
            .height = context.ortho_h,
        };
    }

    context.scene->scene_id = scene_id;

    WPShaderParser::InitGlslang();
    WPTextRenderer::Init();

    for (WPObjectVar& obj : wp_objs) {
        std::visit(visitor::overload {
                       [&context](wpscene::WPImageObject& obj) {                           
                            ParseImageObj(context, obj);
                       },
                       [&context](wpscene::WPParticleObject& obj) {
                           ParseParticleObj(context, obj);
                       },
                       [&context, &sm](wpscene::WPSoundObject& obj) {
                           WPSoundParser::Parse(obj, *context.vfs, sm);
                       },
                       [&context](wpscene::WPLightObject& obj) {
                           ParseLightObj(context, obj);
                       },
                       [&context](wpscene::WPModelObject& obj) {
                           ParseModelObj(context, obj);
                       },
                       [&context](wpscene::WPTextObject& obj) {
                           ParseTextObj(context, obj);
                       },
                   },
                   obj);
    }

    // Record user property → visibility bindings by scanning raw JSON
    for (auto& obj : json.at("objects")) {
        if (! obj.contains("id") || ! obj.at("id").is_number_integer()) continue;
        i32 id = obj.at("id").get<i32>();
        auto node_it = context.node_map.find(id);
        if (node_it == context.node_map.end()) continue;

        // Register node in ID lookup table
        context.scene->nodeById[id] = node_it->second.get();

        // Check if "visible" references a user property
        if (obj.contains("visible") && obj.at("visible").is_object() &&
            obj.at("visible").contains("user")) {
            const auto& userField = obj.at("visible").at("user");
            std::string propName;
            if (userField.is_string()) {
                propName = userField.get<std::string>();
            } else if (userField.is_object() && userField.contains("name")) {
                propName = userField.at("name").get<std::string>();
            }
            if (! propName.empty()) {
                bool defaultVis = true;
                if (obj.at("visible").contains("value")) {
                    defaultVis = obj.at("visible").at("value").get<bool>();
                }
                context.scene->userPropVisBindings[propName].push_back(
                    { node_it->second.get(), defaultVis });
                LOG_INFO("user prop binding: '%s' -> visibility of node id=%d (default=%d)",
                         propName.c_str(), id, (int)defaultVis);
            }
        }
    }

    // Sort root children to match JSON "objects" array order.
    // The two-pass construction (groups first, then images) breaks the intended
    // Z-order.  Re-sorting ensures groups are interleaved with images at their
    // original position so e.g. backgrounds render before compose layers.
    // Camera nodes (not in JSON objects) stay at the front.
    context.scene->sceneGraph->GetChildren().sort(
        [&json_order](const std::shared_ptr<SceneNode>& a,
                       const std::shared_ptr<SceneNode>& b) {
            auto it_a = json_order.find(a->ID());
            auto it_b = json_order.find(b->ID());
            bool has_a = it_a != json_order.end();
            bool has_b = it_b != json_order.end();
            if (! has_a && ! has_b) return false; // both cameras: stable
            if (! has_a) return true;              // cameras stay first
            if (! has_b) return false;
            return it_a->second < it_b->second;
        });

    // Create bloom post-processing passes if enabled
    if (sc.general.bloom) {
        LOG_INFO("Bloom enabled: strength=%.2f threshold=%.2f",
                 sc.general.bloomstrength, sc.general.bloomthreshold);

        auto& scene = *context.scene;
        auto& vfs   = *context.vfs;

        // Create bloom render targets
        scene.renderTargets[std::string(WE_BLOOM_SCENE)] = {
            .width  = context.ortho_w,
            .height = context.ortho_h,
            .bind   = { .enable = true, .screen = true },
        };
        scene.renderTargets[std::string(WE_BLOOM_QUARTER)] = {
            .width  = 2,
            .height = 2,
            .bind   = { .enable = true, .screen = true, .scale = 0.25 },
        };
        scene.renderTargets[std::string(WE_BLOOM_EIGHTH)] = {
            .width  = 2,
            .height = 2,
            .bind   = { .enable = true, .screen = true, .scale = 0.125 },
        };
        scene.renderTargets[std::string(WE_BLOOM_RESULT)] = {
            .width  = 2,
            .height = 2,
            .bind   = { .enable = true, .screen = true, .scale = 0.125 },
        };

        // Bloom pass definitions: shader, input textures, output RT
        struct BloomPassDef {
            std::string              shader;
            std::vector<std::string> textures;
            std::string              output;
        };
        std::array<BloomPassDef, 4> bloomPasses {{
            { "downsample_quarter_bloom",
              { std::string(WE_BLOOM_SCENE) },
              std::string(WE_BLOOM_QUARTER) },
            { "downsample_eighth_blur_v",
              { std::string(WE_BLOOM_QUARTER) },
              std::string(WE_BLOOM_EIGHTH) },
            { "blur_h_bloom",
              { std::string(WE_BLOOM_EIGHTH) },
              std::string(WE_BLOOM_RESULT) },
            { "combine",
              { std::string(WE_BLOOM_SCENE), std::string(WE_BLOOM_RESULT) },
              std::string(SpecTex_Default) },
        }};

        scene.bloomConfig.enabled   = true;
        scene.bloomConfig.strength  = sc.general.bloomstrength;
        scene.bloomConfig.threshold = sc.general.bloomthreshold;

        for (auto& def : bloomPasses) {
            wpscene::WPMaterial wpmat;
            wpmat.shader   = def.shader;
            wpmat.textures = def.textures;

            // Pass bloom parameters to downsample_quarter_bloom (first pass extracts bright pixels)
            if (def.shader == "downsample_quarter_bloom") {
                wpmat.constantshadervalues["bloomstrength"]  = { sc.general.bloomstrength };
                wpmat.constantshadervalues["bloomthreshold"] = { sc.general.bloomthreshold };
            }

            auto          spNode = std::make_shared<SceneNode>();
            WPShaderInfo  shaderInfo;
            shaderInfo.baseConstSvs = context.global_base_uniforms;
            SceneMaterial material;
            WPShaderValueData svData;

            if (! LoadMaterial(vfs, wpmat, &scene, spNode.get(), &material, &svData, &shaderInfo)) {
                LOG_ERROR("bloom: failed to load material for '%s'", def.shader.c_str());
                scene.bloomConfig.enabled = false;
                break;
            }

            LoadConstvalue(material, wpmat, shaderInfo);

            auto spMesh = std::make_shared<SceneMesh>();
            spMesh->AddMaterial(std::move(material));
            spMesh->ChangeMeshDataFrom(scene.default_effect_mesh);
            spNode->AddMesh(spMesh);
            spNode->SetCamera("effect");

            context.shader_updater->SetNodeData(spNode.get(), svData);

            scene.bloomConfig.outputs.push_back(def.output);
            scene.bloomConfig.nodes.push_back(spNode);

            LOG_INFO("bloom: pass '%s' → '%s' created", def.shader.c_str(), def.output.c_str());
        }
    }

    WPShaderParser::FinalGlslang();
    WPTextRenderer::Shutdown();
    return context.scene;
}
