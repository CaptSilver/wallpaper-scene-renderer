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

    // Original world transforms before effect-chain identity reset.
    // When image nodes with effects have their world-node transforms reset
    // to identity, children that inherit_parent lose the parent chain.
    // This map preserves each node's original world transform (accumulated
    // from the parent chain) so proxy nodes can be created for correct
    // child placement.
    std::map<i32, Eigen::Matrix4d> original_world_transforms;
};

using WPObjectVar =
    std::variant<wpscene::WPImageObject, wpscene::WPParticleObject, wpscene::WPSoundObject,
                 wpscene::WPLightObject, wpscene::WPModelObject, wpscene::WPTextObject>;

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

// GS particle: 1 vertex per particle (geometry shader expands to quad)
// When GS_ENABLED, vertex shader packs all rotation into a_TexCoordVec4.xyz
// and does NOT use a_TexCoordC2, so we omit it from the layout.
void SetParticleMeshGS(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
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
    // 1 vertex per particle, no index buffer (POINT_LIST topology)
    mesh.AddVertexArray(SceneVertexArray(attrs, count));
    mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
    mesh.GetVertexArray(0).SetOption(WE_CB_GEOMETRY_SHADER, true);
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

// GS rope: 1 vertex per segment (geometry shader expands to triangle strip)
void SetRopeParticleMeshGS(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
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
    } else {
        attrs.push_back({ WE_IN_TEXCOORDVEC3C2.data(), VertexType::FLOAT4 });
    }
    attrs.push_back({ WE_IN_COLOR.data(), VertexType::FLOAT4 });
    // 1 vertex per segment, no index buffer (POINT_LIST topology)
    mesh.AddVertexArray(SceneVertexArray(attrs, count));
    mesh.GetVertexArray(0).SetOption(WE_PRENDER_ROPE, true);
    mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
    mesh.GetVertexArray(0).SetOption(WE_CB_GEOMETRY_SHADER, true);
}

void SetSpriteTrailMesh(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
                        u32 trail_segments, bool thick_format) {
    (void)particle;
    u32                                                 total_segments = count * trail_segments;
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
    mesh.AddVertexArray(SceneVertexArray(attrs, total_segments * 4));
    mesh.AddIndexArray(SceneIndexArray(total_segments));
    mesh.GetVertexArray(0).SetOption(WE_PRENDER_SPRITETRAIL, true);
    mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
}

void SetSpriteTrailMeshGS(SceneMesh& mesh, const wpscene::Particle& particle, uint32_t count,
                          u32 trail_segments, bool thick_format) {
    (void)particle;
    u32                                                 total_segments = count * trail_segments;
    std::vector<SceneVertexArray::SceneVertexAttribute> attrs {
        { WE_IN_POSITIONVEC4.data(), VertexType::FLOAT4 },
        { WE_IN_TEXCOORDVEC4.data(), VertexType::FLOAT4 },
        { WE_IN_TEXCOORDVEC4C1.data(), VertexType::FLOAT4 },
    };
    if (thick_format) {
        attrs.push_back({ WE_IN_TEXCOORDVEC4C2.data(), VertexType::FLOAT4 });
        attrs.push_back({ WE_IN_TEXCOORDVEC4C3.data(), VertexType::FLOAT4 });
    } else {
        attrs.push_back({ WE_IN_TEXCOORDVEC3C2.data(), VertexType::FLOAT4 });
    }
    attrs.push_back({ WE_IN_COLOR.data(), VertexType::FLOAT4 });
    mesh.AddVertexArray(SceneVertexArray(attrs, total_segments));
    mesh.GetVertexArray(0).SetOption(WE_PRENDER_SPRITETRAIL, true);
    mesh.GetVertexArray(0).SetOption(WE_CB_THICK_FORMAT, thick_format);
    mesh.GetVertexArray(0).SetOption(WE_CB_GEOMETRY_SHADER, true);
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

void LoadControlPoint(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                      const wpscene::ParticleInstanceoverride& over,
                      const std::array<float, 3>&              object_origin) {
    std::span<ParticleControlpoint> pcs = pSys.Controlpoints();
    Eigen::Vector3d                 origin_vec { array_cast<double>(object_origin).data() };
    usize                           s = std::min(pcs.size(), wp.controlpoints.size());
    for (usize i = 0; i < s; i++) {
        pcs[i].offset = Eigen::Vector3d { array_cast<double>(wp.controlpoints[i].offset).data() };
        pcs[i].link_mouse =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::link_mouse];
        pcs[i].worldspace =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::worldspace];
    }
    // Apply instance override control points
    for (int i = 0; i < 8; i++) {
        if (over.controlpointOverrides[i].active) {
            pcs[i].offset =
                Eigen::Vector3d { array_cast<double>(over.controlpointOverrides[i].offset).data() };
        }
    }
    // Convert worldspace control points to local space
    for (usize i = 0; i < pcs.size(); i++) {
        if (pcs[i].worldspace) {
            pcs[i].offset -= origin_vec;
        }
    }
}
void LoadInitializer(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                     const wpscene::ParticleInstanceoverride& over, u32 rope_count = 0,
                     int cp_start = 0) {
    for (const auto& ini : wp.initializers) {
        nlohmann::json iniCopy = ini;
        // Inject/override count for mapsequencebetweencontrolpoints
        if (rope_count > 0 && iniCopy.contains("name") &&
            iniCopy["name"] == "mapsequencebetweencontrolpoints") {
            iniCopy["count"] = rope_count;
        }
        // Inject controlpointstartindex
        if (cp_start > 0) {
            iniCopy["controlpointstartindex"] = cp_start;
        }
        pSys.AddInitializer(WPParticleParser::genParticleInitOp(iniCopy, pSys.Controlpoints()));
    }
    if (over.enabled) pSys.AddInitializer(WPParticleParser::genOverrideInitOp(over));
}
void LoadOperator(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                  const wpscene::ParticleInstanceoverride& over) {
    for (const auto& op : wp.operators) {
        pSys.AddOperator(WPParticleParser::genParticleOperatorOp(op, over));
    }
}
void LoadEmitter(ParticleSubSystem& pSys, const wpscene::Particle& wp, float count, float rate,
                 bool render_rope, u32 rope_batch_size = 1, bool has_periodic = false) {
    bool sort = render_rope;
    for (const auto& em : wp.emitters) {
        auto  newEm      = em;
        float burst_rate = 0.0f;
        if (rope_batch_size > 1 && ! has_periodic && count * rate > 0.001f) {
            // Rope+mapsequence without periodic: rate override controls bolt frequency.
            // Keep base emission rate, use override as burst period.
            burst_rate = 1.0f / (count * rate);
            LOG_INFO("burst emitter: period=%.3fs (rate_override=%.3f, batch=%u)",
                     burst_rate, count * rate, rope_batch_size);
        } else {
            newEm.rate *= count * rate;
        }
        pSys.AddEmitter(
            WPParticleParser::genParticleEmittOp(newEm, sort, rope_batch_size, burst_rate));
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
    } else if (str == "translucent_pa") {
        bm = BlendMode::Translucent_PA;
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

    auto        vert_src = fs::GetFileContent(vfs, shaderPath + ".vert");
    auto        frag_src = fs::GetFileContent(vfs, shaderPath + ".frag");
    std::string geom_src;
    if (vfs.Contains(shaderPath + ".geom"))
        geom_src = fs::GetFileContent(vfs, shaderPath + ".geom");

    // Log first 80 chars of each shader source for debugging
    LOG_INFO("shader '%s' vert(%zu) frag(%zu) geom(%zu)",
             wpmat.shader.c_str(),
             vert_src.size(),
             frag_src.size(),
             geom_src.size());

    bool has_geometry_shader = ! geom_src.empty();
    if (has_geometry_shader) {
        pWPShaderInfo->combos["GS_ENABLED"] = "1";
        LOG_INFO("  geometry shader enabled for '%s'", wpmat.shader.c_str());
    }

    std::vector<WPShaderUnit> sd_units;
    sd_units.push_back(WPShaderUnit {
        .stage           = ShaderType::VERTEX,
        .src             = std::move(vert_src),
        .preprocess_info = {},
    });
    if (has_geometry_shader) {
        sd_units.push_back(WPShaderUnit {
            .stage           = ShaderType::GEOMETRY,
            .src             = std::move(geom_src),
            .preprocess_info = {},
        });
    }
    sd_units.push_back(WPShaderUnit {
        .stage           = ShaderType::FRAGMENT,
        .src             = std::move(frag_src),
        .preprocess_info = {},
    });

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
                             i,
                             name.c_str(),
                             offscreenName.c_str(),
                             rt.width,
                             rt.height,
                             rt.width,
                             rt.height);
                } else if (pScene->renderTargets.count(std::string(SpecTex_Default)) > 0) {
                    const auto& rt = pScene->renderTargets.at(std::string(SpecTex_Default));
                    resolution     = { rt.width, rt.height, rt.width, rt.height };
                    LOG_INFO("  link tex[%zu] '%s' → _rt_default resolution=(%d,%d,%d,%d)",
                             i,
                             name.c_str(),
                             rt.width,
                             rt.height,
                             rt.width,
                             rt.height);
                } else {
                    LOG_ERROR("  link tex[%zu] '%s' → '%s' NOT FOUND",
                              i,
                              name.c_str(),
                              offscreenName.c_str());
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
        if (! exists(sd_units.back().preprocess_info.active_tex_slots, i))
            material.textures[i].clear();
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
                LOG_INFO("  constValue: '%s' -> '%s' = [%s]",
                         name.c_str(),
                         glname.c_str(),
                         valStr.c_str());
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
                 sc.camera.eye[0],
                 sc.camera.eye[1],
                 sc.camera.eye[2],
                 sc.camera.center[0],
                 sc.camera.center[1],
                 sc.camera.center[2],
                 general.fov,
                 aspect,
                 general.nearz,
                 general.farz);

        // Enable 4x MSAA for 3D scenes (perspective camera = 3D models)
        scene.msaaSamples = 4;
        LOG_INFO("MSAA enabled: x%d (3D scene)", scene.msaaSamples);

        // Create orthographic overlay camera for flat image layers in 3D scenes.
        // Image layers default to flat/ortho rendering unless perspective=true.
        // Use the scene's ortho dimensions (pixels) so image/text quad sizes and
        // script-set origins/scales match the same coordinate space as 2D wallpapers.
        {
            float orthoW   = (float)general.orthogonalprojection.width;
            float orthoH   = (float)general.orthogonalprojection.height;
            auto  orthoCam = std::make_shared<SceneCamera>(2, 1, -5000.0f, 5000.0f);
            orthoCam->SetWidth(orthoW);
            orthoCam->SetHeight(orthoH);
            orthoCam->Update();
            auto ortho_node = std::make_shared<SceneNode>();
            orthoCam->AttatchNode(ortho_node);
            scene.sceneGraph->AppendChild(ortho_node);
            scene.cameras["global_ortho"] = orthoCam;
            LOG_INFO("Ortho overlay camera: %.0f x %.0f (for flat image layers)", orthoW, orthoH);
        }

        // Load camera animation paths
        auto&                   vfs = *context.vfs;
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
                        CameraKeyframe       kf;
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
            auto reflCam =
                std::make_shared<SceneCamera>(aspect, general.nearz, general.farz, general.fov);
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
                 context.ortho_w / (i32)general.zoom,
                 context.ortho_h / (i32)general.zoom,
                 cori.x(),
                 cori.y(),
                 general.zoom);

        scene.cameras["global_perspective"] = std::make_shared<SceneCamera>(
            (float)context.ortho_w / (float)context.ortho_h,
            general.nearz,
            general.farz,
            algorism::CalculatePersperctiveFov(1000.0f, context.ortho_h));

        Vector3f cperori = cori;
        cperori[2]       = 1000.0f;
        context.global_perspective_camera_node =
            std::make_shared<SceneNode>(cperori, cscale, cangle);
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

    scene.clearColor = { sc.general.clearcolor[0],
                         sc.general.clearcolor[1],
                         sc.general.clearcolor[2] };
    scene.ortho[0]   = sc.general.orthogonalprojection.width;
    scene.ortho[1]   = sc.general.orthogonalprojection.height;
    context.ortho_w  = scene.ortho[0];
    context.ortho_h  = scene.ortho[1];
    LOG_INFO("Scene ortho: %dx%d (auto=%d) clearColor=(%.3f,%.3f,%.3f)",
             context.ortho_w,
             context.ortho_h,
             sc.general.orthogonalprojection.auto_,
             scene.clearColor[0],
             scene.clearColor[1],
             scene.clearColor[2]);

    {
        auto& gb            = context.global_base_uniforms;
        gb["g_ViewUp"]      = std::array { 0.0f, 1.0f, 0.0f };
        gb["g_ViewRight"]   = std::array { 1.0f, 0.0f, 0.0f };
        gb["g_ViewForward"] = std::array { 0.0f, 0.0f, -1.0f };
        if (! sc.general.isOrtho) {
            gb["g_EyePosition"] = sc.camera.eye;
        } else {
            // Eye position for 2D ortho scenes: place at scene center with Z offset.
            // Without Z offset, rope/trail particle geometry shaders produce degenerate
            // quads (cross product of two XY-plane vectors → Z-only → zero screen width).
            gb["g_EyePosition"] = std::array { (float)context.ortho_w / 2.0f,
                                               (float)context.ortho_h / 2.0f,
                                               1000.0f };
        }
        gb["g_TexelSize"]     = std::array { 1.0f / 1920.0f, 1.0f / 1080.0f };
        gb["g_TexelSizeHalf"] = std::array { 1.0f / 1920.0f / 2.0f, 1.0f / 1080.0f / 2.0f };

        gb["g_LightAmbientColor"]  = sc.general.ambientcolor;
        gb["g_LightSkylightColor"] = sc.general.skylightcolor;
        gb["g_NormalModelMatrix"]  = ShaderValue::fromMatrix(Matrix4f::Identity());
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

    LOG_INFO("ParseImageObj: id=%d name='%s' image='%s' visible=%d size=(%.0f,%.0f) fullscreen=%d "
             "origin=(%.1f,%.1f)",
             wpimgobj.id,
             wpimgobj.name.c_str(),
             wpimgobj.image.c_str(),
             (int)wpimgobj.visible,
             wpimgobj.size[0],
             wpimgobj.size[1],
             (int)wpimgobj.fullscreen,
             wpimgobj.origin[0],
             wpimgobj.origin[1]);

    // Shape-quad size fallback: if no image and size is still default (2,2),
    // use the scene ortho dimensions so effect pingpong RTs have meaningful resolution.
    if (wpimgobj.image.empty() && wpimgobj.size[0] <= 2.0f && wpimgobj.size[1] <= 2.0f) {
        wpimgobj.size = { (float)context.ortho_w, (float)context.ortho_h };
        LOG_INFO("  shape-quad size fallback: %dx%d", context.ortho_w, context.ortho_h);
    }

    // Invisible nodes are processed as offscreen dependency nodes:
    // their output is written to a per-node RT (not the main scene output) so
    // that compose layers can reference it via _rt_imageLayerComposite_XXX_a → _rt_link_XXX.
    // Exception: combo-selector visibility (e.g. character picker with condition values)
    // must stay in the main render graph so variants can be toggled at runtime.
    // Bool user props (clock, music, light) keep normal offscreen routing since
    // they participate in compositing when visible.
    bool isOffscreen = ! wpimgobj.visible && ! wpimgobj.visibleIsComboSelector;

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
        LOG_INFO("  colorBlendMode=%d hw_override=%d",
                 wpimgobj.colorBlendMode,
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
        } else if (puppet->puppet->bones.size() == 0) {
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
    // Only set m_visible for combo-selector nodes — they need runtime toggling.
    // Other invisible nodes keep m_visible=true; isOffscreen controls rendering.
    // Setting m_visible=false would cascade to effect chain nodes via
    // m_visibilityOwner, preventing them from rendering to their offscreen RTs.
    if (wpimgobj.visibleIsComboSelector) {
        spImgNode->SetVisible(wpimgobj.visible);
    }
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

        baseConstSvs["g_Color4"] = std::array<float, 4> {
            wpimgobj.color[0], wpimgobj.color[1], wpimgobj.color[2], wpimgobj.alpha
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
            LOG_ERROR(
                "load imageobj id=%d '%s' material failed", wpimgobj.id, wpimgobj.name.c_str());
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
                 wpimgobj.id,
                 (int)imgBlendMode,
                 isCompose,
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
                     propName.c_str(),
                     glname.c_str(),
                     wpimgobj.id);
        }
    }

    // Record color script for SceneScript evaluation
    if (! wpimgobj.colorScript.empty() && spMesh->Material()) {
        SceneColorScript csi;
        csi.id               = wpimgobj.id;
        csi.material         = spMesh->Material();
        csi.script           = wpimgobj.colorScript;
        csi.scriptProperties = wpimgobj.colorScriptProperties;
        csi.initialColor     = wpimgobj.color;
        context.scene->colorScripts.push_back(std::move(csi));
        LOG_INFO("  color script registered for id=%d", wpimgobj.id);
    }

    context.shader_updater->SetNodeData(spImgNode.get(), svData);

    // Compute and save this node's original world transform before effects
    // potentially reset it to identity.  Children that inherit_parent use
    // the parent's saved transform via a proxy node.
    {
        Eigen::Matrix4d local = spImgNode->GetLocalTrans();
        if (wpimgobj.parent_id >= 0 &&
            context.original_world_transforms.count(wpimgobj.parent_id)) {
            context.original_world_transforms[wpimgobj.id] =
                context.original_world_transforms[wpimgobj.parent_id] * local;
        } else {
            context.original_world_transforms[wpimgobj.id] = local;
        }
    }

    if (hasEffect) {
        auto& scene = *context.scene;
        // currently use addr for unique
        std::string nodeAddr = getAddr(spImgNode.get());
        // set camera to attatch effect
        if (isCompose) {
            // For compose effects, use scene ortho dimensions (works for both
            // ortho and perspective scenes; perspective activeCamera Width/Height
            // defaults to 1.0 which is too small).
            i32 cw = context.ortho_w > 0 ? context.ortho_w : (i32)scene.activeCamera->Width();
            i32 ch = context.ortho_h > 0 ? context.ortho_h : (i32)scene.activeCamera->Height();
            scene.cameras[nodeAddr] = std::make_shared<SceneCamera>(cw, ch, -1.0f, 1.0f);
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
            imgEffectLayer->SetInheritParent(! isOffscreen && wpimgobj.parent_id >= 0 &&
                                             context.node_map.count(wpimgobj.parent_id) > 0);
            // Create proxy node with parent's baked world transform.
            // Parent world nodes may have been reset to identity for their own
            // effect chains, so the live parent chain no longer carries the
            // correct transform.  The proxy preserves it.
            if (! isOffscreen && wpimgobj.parent_id >= 0 &&
                context.original_world_transforms.count(wpimgobj.parent_id)) {
                auto proxy = std::make_shared<SceneNode>();
                proxy->SetWorldTransform(context.original_world_transforms[wpimgobj.parent_id]);
                imgEffectLayer->SetParentProxy(std::move(proxy));
            }
            imgEffectLayer->FinalMesh().ChangeMeshDataFrom(effct_final_mesh);
            imgEffectLayer->FinalNode().CopyTrans(*spImgNode);
            imgEffectLayer->FinalNode().SetVisibilityOwner(spImgNode.get());
            // When a shape-quad has Z-rotation, the rotated quad may not cover
            // the full viewport.  Scale the final node so the rotated rectangle
            // still contains every viewport corner (prevents hard-edge artefacts).
            // Only for shape-quads (image.empty()) — regular images should not
            // be enlarged to cover the viewport.
            if (! isOffscreen && wpimgobj.image.empty() && std::abs(wpimgobj.angles[2]) > 0.01f) {
                float theta = std::abs(wpimgobj.angles[2]);
                float ct = std::cos(theta), st = std::sin(theta);
                float cx = wpimgobj.origin[0], cy = wpimgobj.origin[1];
                float vw = (float)context.ortho_w, vh = (float)context.ortho_h;
                // Max distance from quad center to any viewport corner
                float max_hx = std::max(cx, vw - cx);
                float max_hy = std::max(cy, vh - cy);
                // Effective half-dimensions (mesh size × node scale)
                float hw = (wpimgobj.size[0] / 2.0f) * wpimgobj.scale[0];
                float hh = (wpimgobj.size[1] / 2.0f) * wpimgobj.scale[1];
                if (hw > 0.0f && hh > 0.0f) {
                    float s = std::max((max_hx * ct + max_hy * st) / hw,
                                       (max_hx * st + max_hy * ct) / hh);
                    if (s > 1.01f) {
                        s       = std::min(s, 3.0f);
                        auto sc = imgEffectLayer->FinalNode().Scale();
                        imgEffectLayer->FinalNode().SetScale(sc * s);
                        LOG_INFO("  rotation coverage: angle=%.1f° scale=%.3f",
                                 theta * 180.0f / 3.14159265f,
                                 s);
                    }
                }
            }
            if (isCompose) {
            } else {
                spImgNode->CopyTrans(SceneNode());
            }
            scene.cameras.at(nodeAddr)->AttatchImgEffect(imgEffectLayer);
            // Register for property script transform redirection.
            scene.nodeEffectLayerMap[wpimgobj.id] = imgEffectLayer.get();
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
                // colorBlendMode effectpassthrough: base RT already has color+alpha baked in,
                // don't re-apply g_Color4 (would double-count alpha and re-tint)
                if (wpmat.combos.count("BLENDMODE") != 0) {
                    wpEffShaderInfo.baseConstSvs["g_Color4"] =
                        std::array<float, 4> { 1.0f, 1.0f, 1.0f, 1.0f };
                }
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

                // Register user property bindings for effect material uniforms
                if (! wpmat.userShaderBindings.empty() && spMesh->Material()) {
                    for (auto& [propName, shaderConstName] : wpmat.userShaderBindings) {
                        std::string glname;
                        if (wpEffShaderInfo.alias.count(shaderConstName) != 0) {
                            glname = wpEffShaderInfo.alias.at(shaderConstName);
                        } else {
                            for (const auto& el : wpEffShaderInfo.alias) {
                                if (el.second.substr(2) == shaderConstName) {
                                    glname = el.second;
                                    break;
                                }
                            }
                        }
                        if (glname.empty()) glname = shaderConstName;
                        context.scene->userPropUniformBindings[propName].push_back(
                            { spMesh->Material(), glname });
                        LOG_INFO("  effect user prop binding: '%s' -> '%s' on effect of id=%d",
                                 propName.c_str(),
                                 glname.c_str(),
                                 wpimgobj.id);
                    }
                }

                context.shader_updater->SetNodeData(spEffNode.get(), svData);
                spEffNode->SetVisibilityOwner(spImgNode.get());
                imgEffect->nodes.push_back({ matOutRT, spEffNode });
            }

            if (eff_mat_ok) {
                imgEffectLayer->AddEffect(imgEffect);
                LOG_INFO("  effect[%d] '%s' loaded OK (%zu nodes)",
                         i_eff,
                         wpeffobj.name.c_str(),
                         imgEffect->nodes.size());
            } else {
                LOG_ERROR("effect \'%s\' failed to load", wpeffobj.name.c_str());
            }
        }
        LOG_INFO("  ParseImageObj id=%d: %zu effects loaded, isCompose=%d, isOffscreen=%d",
                 wpimgobj.id,
                 imgEffectLayer->EffectCount(),
                 (int)isCompose,
                 (int)isOffscreen);

        // In perspective scenes, flat image layers need the ortho overlay camera
        // for their final composite (not the perspective camera).
        if (scene.cameras.count("global_ortho") && ! wpimgobj.perspective) {
            imgEffectLayer->SetFinalCamera("global_ortho");
        }
    }

    // In perspective scenes, flat image layers without effects use the ortho
    // overlay camera instead of the perspective camera.  This makes SceneScript
    // origin values (UV coordinates in [-0.5, 0.5]) map correctly to screen space.
    if (! hasEffect && context.scene->activeCamera->IsPerspective() && ! wpimgobj.perspective &&
        context.scene->cameras.count("global_ortho")) {
        spImgNode->SetCamera("global_ortho");
    }

    // Invisible nodes without effects still need an offscreen RT so their output
    // can be referenced via link tex by compose layers.
    if (isOffscreen && ! hasEffect) {
        auto& scene                                      = *context.scene;
        scene.renderTargets[GenOffscreenRT(wpimgobj.id)] = {
            .width      = (uint16_t)wpimgobj.size[0],
            .height     = (uint16_t)wpimgobj.size[1],
            .allowReuse = true,
        };
        LOG_INFO("  created offscreen RT '%s' for id=%d (%dx%d)",
                 GenOffscreenRT(wpimgobj.id).c_str(),
                 wpimgobj.id,
                 (int)wpimgobj.size[0],
                 (int)wpimgobj.size[1]);
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
                 wpimgobj.id,
                 wpimgobj.parent_id,
                 (int)isOffscreen);
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
        spNode->ID()   = wppartobj.id;
    }

    wpscene::ParticleInstanceoverride override = wppartobj.instanceoverride;

    auto& particle_obj = *p_particle_obj;
    auto& vfs          = *context.vfs;

    bool is_spawner_only = particle_obj.renderers.empty();
    if (is_spawner_only && particle_obj.children.empty()) {
        LOG_ERROR("particle '%s' has no renderers and no children, skipping",
                  wppartobj.name.c_str());
        return;
    }
    if (is_spawner_only) {
        LOG_INFO("particle '%s' is spawner-only (no renderer, %zu children)",
                 wppartobj.name.c_str(),
                 particle_obj.children.size());

        // Minimal setup: no mesh/material, just particle subsystem for children
        u32  maxcount    = std::min(particle_obj.maxcount, 20000u);
        auto spMesh      = std::make_shared<SceneMesh>(true);
        auto particleSub = std::make_unique<ParticleSubSystem>(
            *context.scene->paritileSys,
            spMesh,
            maxcount,
            1.0,
            child_data.maxcount,
            child_data.probability,
            ParseSpawnType(child_data.type),
            [](const Particle&, const ParticleRawGenSpec&) {
            },
            particle_obj.starttime);

        LoadControlPoint(*particleSub, particle_obj, override, wppartobj.origin);
        LoadEmitter(*particleSub, particle_obj, override.count, override.rate, false, 1);
        LoadInitializer(*particleSub, particle_obj, override, 0, child_data.controlpointstartindex);
        LoadOperator(*particleSub, particle_obj, override);

        for (auto& child : particle_obj.children) {
            ParseParticleObj(context,
                             wppartobj,
                             { .child             = &child,
                               .node_parent       = spNode.get(),
                               .particle_parent   = particleSub.get(),
                               .max_instancecount = child_ptr.max_instancecount });
        }

        if (is_child)
            child_ptr.particle_parent->AddChild(std::move(particleSub));
        else
            context.scene->paritileSys->subsystems.emplace_back(std::move(particleSub));

        if (is_child)
            child_ptr.node_parent->AppendChild(spNode);
        else
            context.scene->sceneGraph->AppendChild(spNode);
        context.node_map[wppartobj.id] = spNode;
        return;
    }
    auto wppartRenderer = particle_obj.renderers.at(0);
    LOG_INFO("particle '%s' renderer='%s'", wppartobj.name.c_str(), wppartRenderer.name.c_str());
    bool render_ropetrail   = (wppartRenderer.name == "ropetrail");
    bool render_rope        = sstart_with(wppartRenderer.name, "rope") && ! render_ropetrail;
    bool render_spritetrail = (wppartRenderer.name == "spritetrail") || render_ropetrail;
    bool hastrail           = render_rope || send_with(wppartRenderer.name, "trail");

    if (render_rope || render_spritetrail) particle_obj.material.shader = "genericropeparticle";

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

    // Check for periodic emitters early (needed for maxcount doubling)
    bool has_periodic = false;
    for (const auto& em : particle_obj.emitters) {
        if (em.maxperiodicduration > 0 || em.maxperiodicdelay > 0) {
            has_periodic = true;
            break;
        }
    }

    u32 maxcount = particle_obj.maxcount;
    maxcount     = std::min(maxcount, 20000u);

    u32 trail_segments =
        render_spritetrail ? std::clamp((u32)wppartRenderer.maxlength, 2u, 64u) : 0;

    if (hastrail) {
        double in_SegmentUVTimeOffset = 0.0;
        double in_SegmentMaxCount     = render_spritetrail ? trail_segments - 1.0 : maxcount - 1.0;
        shaderInfo.baseConstSvs["g_RenderVar0"] = std::array {
            (float)wppartRenderer.length,
            (float)wppartRenderer.maxlength,
            (float)in_SegmentUVTimeOffset,
            (float)in_SegmentMaxCount,
        };
        shaderInfo.combos["THICKFORMAT"]   = "1";
        shaderInfo.combos["TRAILRENDERER"] = "1";
        int subdiv = (int)wppartRenderer.subdivision;
        if (subdiv > 0) {
            shaderInfo.combos["TRAILSUBDIVISION"] = std::to_string(subdiv);
            LOG_INFO("  rope subdivision=%d for '%s'", subdiv, wppartobj.name.c_str());
        }
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

    bool thick_format        = material.hasSprite || hastrail;
    bool has_geometry_shader = exists(shaderInfo.combos, "GS_ENABLED");
    {
        u32 mesh_maxcount = maxcount * (u32)child_ptr.max_instancecount;
        if (render_rope) {
            if (has_geometry_shader)
                SetRopeParticleMeshGS(mesh, particle_obj, mesh_maxcount, thick_format);
            else
                SetRopeParticleMesh(mesh, particle_obj, mesh_maxcount, thick_format);
        } else if (render_spritetrail) {
            if (has_geometry_shader)
                SetSpriteTrailMeshGS(
                    mesh, particle_obj, mesh_maxcount, trail_segments, thick_format);
            else
                SetSpriteTrailMesh(mesh, particle_obj, mesh_maxcount, trail_segments, thick_format);
        } else {
            if (has_geometry_shader) {
                SetParticleMeshGS(mesh, particle_obj, mesh_maxcount, thick_format);
            } else {
                SetParticleMesh(mesh, particle_obj, mesh_maxcount, thick_format);
            }
        }
    }

    auto particleSub = std::make_unique<ParticleSubSystem>(
        *context.scene->paritileSys,
        spMesh,
        maxcount,
        1.0,
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
        },
        particle_obj.starttime);

    if (render_spritetrail) {
        particleSub->SetSpriteTrail(trail_segments, wppartRenderer.length);
    }

    LoadControlPoint(*particleSub, particle_obj, override, wppartobj.origin);

    // Detect batch size for rope particles with mapsequencebetweencontrolpoints
    u32 rope_batch_size = 1;
    if (render_rope) {
        for (const auto& ini : particle_obj.initializers) {
            if (ini.contains("name")) {
                std::string ini_name;
                GET_JSON_NAME_VALUE(ini, "name", ini_name);
                if (ini_name == "mapsequencebetweencontrolpoints") {
                    if (ini.contains("count")) {
                        GET_JSON_NAME_VALUE(ini, "count", rope_batch_size);
                    } else {
                        // Default to maxtoemitperperiod or maxcount
                        for (const auto& em : particle_obj.emitters) {
                            if (em.maxtoemitperperiod > 0) {
                                rope_batch_size = em.maxtoemitperperiod;
                                break;
                            }
                        }
                        if (rope_batch_size <= 1) rope_batch_size = particle_obj.maxcount;
                    }
                    break;
                }
            }
        }
    }
    // For periodic emitters, don't batch — let particles emit gradually so
    // the rope grows from one end to the other instead of appearing instantly.
    u32 emitter_batch = rope_batch_size;
    if (render_rope && rope_batch_size > 1)
        LOG_INFO("rope '%s' batch_size=%u emitter_batch=%u maxcount=%u periodic=%d",
                 wppartobj.name.c_str(),
                 rope_batch_size,
                 emitter_batch,
                 particle_obj.maxcount,
                 has_periodic);
    LoadEmitter(*particleSub,
                particle_obj,
                override.count,
                override.rate,
                render_rope && ! render_spritetrail,
                emitter_batch,
                has_periodic);
    // For rope mapsequence: use maxtoemitperperiod as count so each particle gets
    // a unique position along the line (avoids half-filled rope from maxcount mismatch)
    u32 rope_init_count = 0;
    if (render_rope) {
        for (const auto& em : particle_obj.emitters) {
            if (em.maxtoemitperperiod > 0) {
                rope_init_count = em.maxtoemitperperiod;
                break;
            }
        }
        if (rope_init_count == 0) rope_init_count = maxcount;
    }
    LoadInitializer(
        *particleSub, particle_obj, override, rope_init_count, child_data.controlpointstartindex);
    LoadOperator(*particleSub, particle_obj, override);

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
             model_obj.id,
             model_obj.name.c_str(),
             model_obj.model.c_str());

    WPMdl mdl;
    if (! WPMdlParser::Parse(model_obj.model, vfs, mdl)) {
        LOG_ERROR("parse model failed: %s", model_obj.model.c_str());
        return;
    }

    // Parent node holds the transform; submesh nodes are children
    auto spParent  = std::make_shared<SceneNode>(Vector3f(model_obj.origin.data()),
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

        // 3D models default to depth enabled, but respect explicit overrides
        // from the material JSON (e.g. skybox sets depthtest/depthwrite to "disabled").
        // WE materials use both "depthtest"/"depthwrite" and the alternate spellings
        // "depthtesting"/"depthwriting" — check for both.
        const auto& jContent = jMat.at("passes").at(0);
        if (! jContent.contains("depthtest") && ! jContent.contains("depthtesting")) {
            wpmat.depthtest = "enabled";
        }
        if (! jContent.contains("depthwrite") && ! jContent.contains("depthwriting")) {
            wpmat.depthwrite = "enabled";
        }

        // Materials that need transparency explicitly set "blending": "translucent".
        // Without it, default to opaque for 3D models (WPMaterial default is translucent for 2D).
        if (! jContent.contains("blending")) {
            wpmat.blending = "normal";
        }
        // Translucent/additive materials: keep depth test but disable depth write so
        // objects behind transparent/additive surfaces remain visible.
        if (wpmat.blending == "translucent" || wpmat.blending == "additive") {
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

        if (! LoadMaterial(
                vfs, wpmat, context.scene.get(), spNode.get(), &material, &svData, &shaderInfo)) {
            LOG_ERROR("load model id=%d submesh %zu '%s' material failed",
                      model_obj.id,
                      si,
                      sub.mat_json_file.c_str());
            continue;
        }
        LOG_INFO("  submesh[%zu] shader '%s' mat='%s' verts=%zu tris=%zu blend=%s",
                 si,
                 wpmat.shader.c_str(),
                 sub.mat_json_file.c_str(),
                 sub.vertexs.size(),
                 sub.indices.size(),
                 wpmat.blending.c_str());
        LoadConstvalue(material, wpmat, shaderInfo);

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
    LOG_INFO("  ParseModelObj id=%d completed, %zu submeshes", model_obj.id, mdl.submeshes.size());
}

void ParseTextObj(ParseContext& context, wpscene::WPTextObject& textObj) {
    auto& vfs = *context.vfs;

    LOG_INFO("ParseTextObj: id=%d name='%s' font='%s' text='%s' visible=%d",
             textObj.id,
             textObj.name.c_str(),
             textObj.font.c_str(),
             textObj.textValue.c_str(),
             (int)textObj.visible);

    if (! textObj.visible) {
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
    auto textImage = WPTextRenderer::RenderText(fontData,
                                                textObj.pointsize,
                                                textObj.textValue,
                                                texW,
                                                texH,
                                                textObj.horizontalalign,
                                                textObj.verticalalign,
                                                textObj.padding);
    if (! textImage) {
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
    stex.url                        = texKey;
    stex.sample                     = { TextureWrap::CLAMP_TO_EDGE,
                                        TextureWrap::CLAMP_TO_EDGE,
                                        TextureFilter::LINEAR,
                                        TextureFilter::LINEAR };
    context.scene->textures[texKey] = stex;

    // Build material: genericimage2 shader with text texture
    wpscene::WPMaterial wpMat;
    wpMat.shader   = "genericimage2";
    wpMat.blending = "translucent";
    wpMat.textures.push_back(texKey);

    // Create scene node
    auto spNode  = std::make_shared<SceneNode>(Vector3f(textObj.origin.data()),
                                              Vector3f(textObj.scale.data()),
                                              Vector3f(textObj.angles.data()));
    spNode->ID() = textObj.id;

    // Load material
    SceneMaterial     material;
    WPShaderValueData svData;
    WPShaderInfo      shaderInfo;
    shaderInfo.baseConstSvs             = context.global_base_uniforms;
    shaderInfo.baseConstSvs["g_Color4"] = std::array<float, 4> {
        textObj.color[0], textObj.color[1], textObj.color[2], textObj.alpha
    };
    shaderInfo.baseConstSvs["g_UserAlpha"]  = textObj.alpha;
    shaderInfo.baseConstSvs["g_Brightness"] = textObj.brightness;

    if (! LoadMaterial(
            vfs, wpMat, context.scene.get(), spNode.get(), &material, &svData, &shaderInfo)) {
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
        auto&       scene    = *context.scene;
        std::string nodeAddr = getAddr(spNode.get());

        i32 w                   = texW;
        i32 h                   = texH;
        scene.cameras[nodeAddr] = std::make_shared<SceneCamera>(w, h, -1.0f, 1.0f);
        scene.cameras.at(nodeAddr)->AttatchNode(context.effect_camera_node);
        spNode->SetCamera(nodeAddr);

        std::string effect_ppong_a = WE_EFFECT_PPONG_PREFIX_A.data() + nodeAddr;
        std::string effect_ppong_b = WE_EFFECT_PPONG_PREFIX_B.data() + nodeAddr;

        SceneMesh effctFinalMesh {};
        GenCardMesh(effctFinalMesh, { static_cast<uint16_t>(w), static_cast<uint16_t>(h) });

        auto imgEffectLayer = std::make_shared<SceneImageEffectLayer>(spNode.get(),
                                                                      static_cast<float>(w),
                                                                      static_cast<float>(h),
                                                                      effect_ppong_a,
                                                                      effect_ppong_b);
        {
            imgEffectLayer->SetFinalBlend(imgBlendMode);
            imgEffectLayer->FinalMesh().ChangeMeshDataFrom(effctFinalMesh);
            imgEffectLayer->FinalNode().CopyTrans(*spNode);
            imgEffectLayer->FinalNode().SetVisibilityOwner(spNode.get());
            spNode->CopyTrans(SceneNode());
            scene.cameras.at(nodeAddr)->AttatchImgEffect(imgEffectLayer);
            // Register for property script transform redirection.
            scene.nodeEffectLayerMap[spNode->ID()] = imgEffectLayer.get();
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
            if (! wpeffobj.visible) {
                i_eff--;
                continue;
            }

            auto              imgEffect = std::make_shared<SceneImageEffect>();
            const std::string inRT { effect_ppong_a };

            std::string                                  effaddr = getAddr(imgEffectLayer.get());
            std::unordered_map<std::string, std::string> fboMap;
            fboMap["previous"] = inRT;
            for (usize i = 0; i < wpeffobj.fbos.size(); i++) {
                const auto& wpfbo  = wpeffobj.fbos.at(i);
                std::string rtname = std::string(WE_SPEC_PREFIX) + wpfbo.name + "_" + effaddr;
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
                std::string         matOutRT { WE_EFFECT_PPONG_PREFIX_B };
                if (wpeffobj.passes.size() > i_mat) {
                    const auto& wppass = wpeffobj.passes.at(i_mat);
                    wpmat.MergePass(wppass);
                    for (const auto& el : wppass.bind) {
                        if (fboMap.count(el.name) == 0) continue;
                        if (wpmat.textures.size() <= static_cast<usize>(el.index))
                            wpmat.textures.resize(static_cast<usize>(el.index) + 1);
                        wpmat.textures[static_cast<usize>(el.index)] = fboMap[el.name];
                    }
                    if (! wppass.target.empty() && fboMap.count(wppass.target))
                        matOutRT = fboMap.at(wppass.target);
                }
                if (wpmat.textures.empty()) wpmat.textures.resize(1);
                if (wpmat.textures.at(0).empty()) wpmat.textures[0] = inRT;

                auto         spEffNode = std::make_shared<SceneNode>();
                WPShaderInfo wpEffShaderInfo;
                wpEffShaderInfo.baseConstSvs = baseConstSvs;
                wpEffShaderInfo.baseConstSvs["g_EffectTextureProjectionMatrix"] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());
                wpEffShaderInfo.baseConstSvs["g_EffectTextureProjectionMatrixInverse"] =
                    ShaderValue::fromMatrix(Eigen::Matrix4f::Identity());

                SceneMaterial     effMaterial;
                WPShaderValueData effSvData;
                if (! LoadMaterial(vfs,
                                   wpmat,
                                   context.scene.get(),
                                   spEffNode.get(),
                                   &effMaterial,
                                   &effSvData,
                                   &wpEffShaderInfo)) {
                    eff_mat_ok = false;
                    break;
                }
                LoadConstvalue(effMaterial, wpmat, wpEffShaderInfo);
                auto spEffMesh = std::make_shared<SceneMesh>();
                spEffMesh->AddMaterial(std::move(effMaterial));
                spEffNode->AddMesh(spEffMesh);

                // Register user property bindings for text effect material uniforms
                if (! wpmat.userShaderBindings.empty() && spEffMesh->Material()) {
                    for (auto& [propName, shaderConstName] : wpmat.userShaderBindings) {
                        std::string glname;
                        if (wpEffShaderInfo.alias.count(shaderConstName) != 0) {
                            glname = wpEffShaderInfo.alias.at(shaderConstName);
                        } else {
                            for (const auto& el : wpEffShaderInfo.alias) {
                                if (el.second.substr(2) == shaderConstName) {
                                    glname = el.second;
                                    break;
                                }
                            }
                        }
                        if (glname.empty()) glname = shaderConstName;
                        context.scene->userPropUniformBindings[propName].push_back(
                            { spEffMesh->Material(), glname });
                    }
                }

                context.shader_updater->SetNodeData(spEffNode.get(), effSvData);
                spEffNode->SetVisibilityOwner(spNode.get());
                imgEffect->nodes.push_back({ matOutRT, spEffNode });
            }
            if (eff_mat_ok) imgEffectLayer->AddEffect(imgEffect);
        }
    }

    // Register text layer info for dynamic script evaluation (Phase 2)
    if (! textObj.textScript.empty()) {
        TextLayerInfo tli;
        tli.id                = textObj.id;
        tli.fontData          = fontData;
        tli.pointsize         = textObj.pointsize;
        tli.texWidth          = texW;
        tli.texHeight         = texH;
        tli.padding           = textObj.padding;
        tli.halign            = textObj.horizontalalign;
        tli.valign            = textObj.verticalalign;
        tli.currentText       = textObj.textValue;
        tli.textureKey        = texKey;
        tli.script            = textObj.textScript;
        tli.scriptProperties  = textObj.textScriptProperties;
        tli.pointsizeUserProp = textObj.pointsizeUserProp;
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
                                            const WPUserProperties& userProps) {
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
    struct GroupInfo {
        i32 id;
        i32 parent_id;
    };
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

                LOG_INFO(
                    "created group node id=%d origin=(%.1f, %.1f, %.1f) scale=(%.1f, %.1f, %.1f)",
                    gid,
                    origin[0],
                    origin[1],
                    origin[2],
                    scale[0],
                    scale[1],
                    scale[2]);
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
    // Compute original world transforms for group nodes so image-object
    // children can look up the correct parent transform even when the
    // parent's world node has been reset to identity for effect rendering.
    for (auto& gi : group_infos) {
        auto& node  = context.node_map.at(gi.id);
        auto  local = node->GetLocalTrans();
        if (gi.parent_id >= 0 && context.original_world_transforms.count(gi.parent_id)) {
            context.original_world_transforms[gi.id] =
                context.original_world_transforms[gi.parent_id] * local;
        } else {
            context.original_world_transforms[gi.id] = local;
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
        context.scene->renderTargets[WE_REFLECTION_BLUR.data()] = {
            .width  = context.ortho_w,
            .height = context.ortho_h,
        };
    }

    context.scene->scene_id = scene_id;

    WPShaderParser::InitGlslang();
    WPTextRenderer::Init();

    // Build name→initial state from parsed objects (before node transforms may be reset)
    struct ObjInitState {
        std::array<float, 3> origin;
        std::array<float, 3> scale;
        std::array<float, 3> angles;
        std::array<float, 2> size;
        bool                 visible;
    };
    std::unordered_map<std::string, ObjInitState> nameToObjState;
    for (WPObjectVar& obj : wp_objs) {
        std::visit(visitor::overload {
                       [&context, &nameToObjState](wpscene::WPImageObject& obj) {
                           if (! obj.name.empty()) {
                               nameToObjState[obj.name] = {
                                   obj.origin, obj.scale, obj.angles, obj.size, obj.visible
                               };
                           }
                           ParseImageObj(context, obj);
                       },
                       [&context](wpscene::WPParticleObject& obj) {
                           ParseParticleObj(context, obj);
                       },
                       [&context, &sm](wpscene::WPSoundObject& obj) {
                           auto* streamPtr = WPSoundParser::Parse(obj, *context.vfs, sm);
                           if (streamPtr) {
                               if (obj.hasVolumeScript) {
                                   Scene::SoundVolumeScript svs;
                                   svs.script           = obj.volumeScript;
                                   svs.scriptProperties = obj.volumeScriptProperties;
                                   svs.initialVolume    = obj.volume;
                                   svs.streamPtr        = streamPtr;
                                   context.scene->soundVolumeScripts.push_back(std::move(svs));
                               }
                               // Register sound layer for SceneScript play/stop/pause API
                               Scene::SoundLayerInfo sli;
                               sli.name          = obj.name;
                               sli.initialVolume = obj.volume;
                               sli.startsilent   = obj.startsilent;
                               sli.streamPtr     = streamPtr;
                               context.scene->soundLayers.push_back(std::move(sli));
                               LOG_INFO("sound layer registered: '%s' (startsilent=%d, mode=%s)",
                                        obj.name.c_str(),
                                        (int)obj.startsilent,
                                        obj.playbackmode.c_str());
                           }
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
        i32  id      = obj.at("id").get<i32>();
        auto node_it = context.node_map.find(id);
        if (node_it == context.node_map.end()) continue;

        // Register node in ID lookup table
        context.scene->nodeById[id] = node_it->second.get();

        // Register name → id mapping and initial state for thisScene.getLayer()
        if (obj.contains("name") && obj.at("name").is_string()) {
            std::string name = obj.at("name").get<std::string>();
            if (! name.empty()) {
                context.scene->nodeNameToId[name] = id;
                Scene::LayerInitialState lis;
                // Prefer WPImageObject values (node transforms may be reset by effect chains)
                auto objIt = nameToObjState.find(name);
                if (objIt != nameToObjState.end()) {
                    lis.origin  = objIt->second.origin;
                    lis.scale   = objIt->second.scale;
                    lis.angles  = objIt->second.angles;
                    lis.size    = objIt->second.size;
                    lis.visible = objIt->second.visible;
                } else {
                    auto* node  = node_it->second.get();
                    lis.origin  = { node->Translate().x(),
                                    node->Translate().y(),
                                    node->Translate().z() };
                    lis.scale   = { node->Scale().x(), node->Scale().y(), node->Scale().z() };
                    lis.angles  = { node->Rotation().x(),
                                    node->Rotation().y(),
                                    node->Rotation().z() };
                    lis.visible = node->IsVisible();
                }
                context.scene->layerInitialStates[name] = lis;
            }
        }

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
                // Extract condition value from visible.value
                // For boolean: "value": true/false → conditionValue "1"/"0"
                // For combo:   "value": "6" or "value": 6 → conditionValue "6"
                std::string conditionValue;
                bool        defaultVis = true;
                if (obj.at("visible").contains("value")) {
                    const auto& visValue = obj.at("visible").at("value");
                    if (visValue.is_boolean()) {
                        bool bv        = visValue.get<bool>();
                        conditionValue = bv ? "1" : "0";
                        defaultVis     = bv;
                    } else if (visValue.is_number_integer()) {
                        int iv         = visValue.get<int>();
                        conditionValue = std::to_string(iv);
                        defaultVis     = (iv != 0);
                    } else if (visValue.is_string()) {
                        conditionValue = visValue.get<std::string>();
                        defaultVis     = ! conditionValue.empty() && conditionValue != "0";
                    }
                }
                // Also check "user" object for condition value (alternative format)
                // Format: "user": {"name": "propname", "value": 6}
                if (conditionValue.empty() && userField.is_object() &&
                    userField.contains("value")) {
                    const auto& cv = userField.at("value");
                    if (cv.is_number_integer()) {
                        conditionValue = std::to_string(cv.get<int>());
                    } else if (cv.is_string()) {
                        conditionValue = cv.get<std::string>();
                    }
                }
                // Store raw visible JSON for runtime re-resolution
                std::string rawVisJson = obj.at("visible").dump();
                context.scene->userPropVisBindings[propName].push_back(
                    { node_it->second.get(), defaultVis, conditionValue, rawVisJson });
                LOG_INFO("user prop binding: '%s' -> visibility of node id=%d (default=%d, "
                         "cond='%s') raw=%s",
                         propName.c_str(),
                         id,
                         (int)defaultVis,
                         conditionValue.c_str(),
                         rawVisJson.c_str());
            }
        }
    }

    // Extract property scripts from raw JSON (visible, origin, scale, angles, alpha)
    for (auto& obj : json.at("objects")) {
        if (! obj.contains("id") || ! obj.at("id").is_number_integer()) continue;
        i32 id = obj.at("id").get<i32>();

        for (const char* prop : { "visible", "origin", "scale", "angles", "alpha" }) {
            if (! obj.contains(prop)) continue;
            auto& val = obj.at(prop);
            if (! val.is_object() || ! val.contains("script")) continue;

            ScenePropertyScript sps;
            sps.id       = id;
            sps.property = prop;
            sps.script   = val.at("script").get<std::string>();
            if (obj.contains("name") && obj.at("name").is_string())
                sps.layerName = obj.at("name").get<std::string>();
            if (val.contains("scriptproperties"))
                sps.scriptProperties = val.at("scriptproperties").dump();

            // Parse initial value
            if (std::string(prop) == "visible") {
                sps.initialVisible = val.value("value", true);
            } else if (std::string(prop) == "alpha") {
                sps.initialFloat = val.value("value", 1.0f);
            } else {
                // origin/scale/angles: "x y z" space-separated string
                if (val.contains("value") && val.at("value").is_string()) {
                    std::istringstream iss(val.at("value").get<std::string>());
                    iss >> sps.initialVec3[0] >> sps.initialVec3[1] >> sps.initialVec3[2];
                }
            }
            context.scene->propertyScripts.push_back(std::move(sps));
        }
    }
    if (! context.scene->propertyScripts.empty()) {
        LOG_INFO("Extracted %zu property scripts", context.scene->propertyScripts.size());
    }

    // Sort root children to match JSON "objects" array order.
    // The two-pass construction (groups first, then images) breaks the intended
    // Z-order.  Re-sorting ensures groups are interleaved with images at their
    // original position so e.g. backgrounds render before compose layers.
    // Camera nodes (not in JSON objects) stay at the front.
    context.scene->sceneGraph->GetChildren().sort(
        [&json_order](const std::shared_ptr<SceneNode>& a, const std::shared_ptr<SceneNode>& b) {
            auto it_a  = json_order.find(a->ID());
            auto it_b  = json_order.find(b->ID());
            bool has_a = it_a != json_order.end();
            bool has_b = it_b != json_order.end();
            if (! has_a && ! has_b) return false; // both cameras: stable
            if (! has_a) return true;             // cameras stay first
            if (! has_b) return false;
            return it_a->second < it_b->second;
        });

    // Create bloom post-processing passes if enabled
    if (sc.general.bloom) {
        LOG_INFO("Bloom enabled: strength=%.2f threshold=%.2f",
                 sc.general.bloomstrength,
                 sc.general.bloomthreshold);

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
        std::array<BloomPassDef, 4> bloomPasses { {
            { "downsample_quarter_bloom",
              { std::string(WE_BLOOM_SCENE) },
              std::string(WE_BLOOM_QUARTER) },
            { "downsample_eighth_blur_v",
              { std::string(WE_BLOOM_QUARTER) },
              std::string(WE_BLOOM_EIGHTH) },
            { "blur_h_bloom", { std::string(WE_BLOOM_EIGHTH) }, std::string(WE_BLOOM_RESULT) },
            { "combine",
              { std::string(WE_BLOOM_SCENE), std::string(WE_BLOOM_RESULT) },
              std::string(SpecTex_Default) },
        } };

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

            auto         spNode = std::make_shared<SceneNode>();
            WPShaderInfo shaderInfo;
            shaderInfo.baseConstSvs = context.global_base_uniforms;
            SceneMaterial     material;
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

    // Reflection blur: two-pass separable Gaussian blur on _rt_Reflection
    if (context.scene->cameras.count("reflected_perspective") > 0) {
        auto& scene = *context.scene;
        auto& vfs   = *context.vfs;
        struct ReflBlurDef {
            std::string              shader;
            std::vector<std::string> textures;
            std::string              output;
            int32_t                  vertical;
        };
        std::array<ReflBlurDef, 2> reflBlurPasses { {
            { "blur_k3", { std::string(WE_REFLECTION) }, std::string(WE_REFLECTION_BLUR), 0 },
            { "blur_k3", { std::string(WE_REFLECTION_BLUR) }, std::string(WE_REFLECTION), 1 },
        } };

        for (auto& def : reflBlurPasses) {
            wpscene::WPMaterial wpmat;
            wpmat.shader             = def.shader;
            wpmat.textures           = def.textures;
            wpmat.combos["VERTICAL"] = def.vertical;

            auto         spNode = std::make_shared<SceneNode>();
            WPShaderInfo shaderInfo;
            shaderInfo.baseConstSvs = context.global_base_uniforms;
            SceneMaterial     material;
            WPShaderValueData svData;

            if (! LoadMaterial(vfs, wpmat, &scene, spNode.get(), &material, &svData, &shaderInfo)) {
                LOG_ERROR("reflection blur: failed to load material for '%s' VERTICAL=%d",
                          def.shader.c_str(),
                          def.vertical);
                scene.reflectionBlurConfig.nodes.clear();
                scene.reflectionBlurConfig.outputs.clear();
                break;
            }

            LoadConstvalue(material, wpmat, shaderInfo);

            auto spMesh = std::make_shared<SceneMesh>();
            spMesh->AddMaterial(std::move(material));
            spMesh->ChangeMeshDataFrom(scene.default_effect_mesh);
            spNode->AddMesh(spMesh);
            spNode->SetCamera("effect");

            context.shader_updater->SetNodeData(spNode.get(), svData);

            scene.reflectionBlurConfig.outputs.push_back(def.output);
            scene.reflectionBlurConfig.nodes.push_back(spNode);

            LOG_INFO("reflection blur: pass '%s' VERTICAL=%d → '%s' created",
                     def.shader.c_str(),
                     def.vertical,
                     def.output.c_str());
        }
    }

    // Wait for all deferred async shader compilations
    WPShaderParser::FlushPendingCompilations(*context.vfs);

    WPShaderParser::FinalGlslang();
    WPTextRenderer::Shutdown();
    return context.scene;
}
