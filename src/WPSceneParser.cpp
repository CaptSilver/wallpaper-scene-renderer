#include "WPSceneParser.hpp"
#include "WPJson.hpp"
#include "WPPropertyScriptExtract.hpp"
#include "WPRopeCombos.hpp"
#include "WPUserProperties.hpp"

#include "Utils/String.h"
#include "Utils/Logging.h"
#include "Utils/Algorism.h"
#include "Utils/SceneProfiler.h"
#include "Core/Visitors.hpp"
#include "Core/StringHelper.hpp"
#include "Core/ArrayHelper.hpp"
#include "SpecTexs.hpp"

#include "WPShaderParser.hpp"
#include "WPTexImageParser.hpp"
#include "WPParticleParser.hpp"
#include "WPMapSequenceParse.hpp"
#include "WPSoundParser.hpp"
#include "WPMdlParser.hpp"
#include "WPSceneAttachmentCompose.hpp"

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
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <random>
#include <cmath>
#include <functional>
#include <regex>
#include <unordered_set>
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

    // Child-attachment world transform for puppet nodes: equals the node's own
    // world * puppet.bone[0].transform (legacy — kept for fallback when a
    // child doesn't name a specific attachment).  For nodes without a
    // puppet this is identical to original_world_transforms.
    std::map<i32, Eigen::Matrix4d> child_attachment_transforms;

    // Per-node puppet pointer, populated when a scene.json object declares
    // "puppet": "models/X.mdl".  Children use this to resolve their
    // "attachment": "<bone-or-attachment-name>" field against the parent's
    // MDAT attachment table.
    std::map<i32, std::shared_ptr<WPPuppet>> node_puppet;

    // Diagnostic-only: parent's MDL vertex bounds and scene.json size, kept
    // so a child's attachment-resolution log can show what coordinate space
    // the MDAT attachment matrix is likely in (MDL pixel vs. scene-size).
    // Layout: {xmin, ymin, zmin, xmax, ymax, zmax}.
    std::map<i32, std::array<float, 6>> node_mdl_bounds;
    std::map<i32, std::array<float, 2>> node_scene_size;
    std::map<i32, std::string>          node_name_for_log;

    // Debug hide-pattern filter (comma-separated substrings).  Objects whose
    // name contains any needle get rendered invisible.  Plumbed in from
    // sceneviewer's --hide-pattern CLI flag.  Empty string disables.
    std::string              hide_pattern;
    std::vector<std::string> hidden_names; // populated for end-of-parse summary

    // Layer names referenced by any SceneScript (via getLayer('X')).  These
    // layers may have their visibility toggled at runtime, so we keep them in
    // the main render graph even if they start with visible=false.  Without
    // this carve-out, invisible layers get routed to an offscreen RT and
    // subsequent SetVisible(true) has no visible effect — see dino_run's
    // jump sprite.
    std::unordered_set<std::string> script_referenced_layers;

    // Asset paths that SceneScript registered via engine.registerAsset('path').
    // A pool of hidden scene nodes gets pre-allocated for each so that
    // thisScene.createLayer(asset) can instantiate them at runtime.
    std::unordered_set<std::string> registered_asset_paths;

    // Workshop id declared by the script that asked for each asset path.
    // When a bare path like 'models/bar.json' doesn't exist on disk, we
    // probe 'models/workshop/<id>/bar.json' as a fallback (workshop
    // imports rebuild a `__workshopId = 'NNN'` declaration at the top of
    // every script — that's our resolver hint).
    std::unordered_map<std::string, std::string> script_asset_workshop_id;

    // Per-asset pool-size hints derived from the enclosing script.  When a
    // script implements its own object pool (pattern: `XxxPool.pop()`), it
    // expects createLayer to eventually return many layers — one per pool
    // entry.  We size the backend pool from the largest integer slider `max:`
    // value in that script (e.g. 3body's `trailLength: {..., max: 400}` × N
    // bodies) so LRU-recycle doesn't have to visibly cycle live trails.
    std::unordered_map<std::string, size_t> asset_pool_size_hints;
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
    // The genericropeparticle shader ALWAYS expects these exact attributes
    // regardless of whether the material defines "thick" ropes.
    attrs.push_back({ WE_IN_TEXCOORDVEC4C2.data(), VertexType::FLOAT4 });
    attrs.push_back({ WE_IN_TEXCOORDVEC4C3.data(), VertexType::FLOAT4 });
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
                      const std::array<float, 3>& object_origin, int32_t cp_start_shift = 0) {
    std::span<ParticleControlpoint> pcs = pSys.Controlpoints();
    Eigen::Vector3d                 origin_vec { array_cast<double>(object_origin).data() };
    usize                           s = std::min(pcs.size(), wp.controlpoints.size());
    for (usize i = 0; i < s; i++) {
        pcs[i].offset = Eigen::Vector3d { array_cast<double>(wp.controlpoints[i].offset).data() };
        pcs[i].link_mouse =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::link_mouse];
        pcs[i].worldspace =
            wp.controlpoints[i].flags[wpscene::ParticleControlpoint::FlagEnum::worldspace];
        pcs[i].follow_parent_particle =
            wp.controlpoints[i]
                .flags[wpscene::ParticleControlpoint::FlagEnum::follow_parent_particle];
        pcs[i].parent_cp_index = wp.controlpoints[i].parentcontrolpoint;
        pcs[i].is_null_offset  = wp.controlpoints[i].offset_is_null;
    }
    // The child-container `controlpointstartindex` is stored verbatim on the subsystem
    // and applied at chain-resolve time.  Keeping the authored `parent_cp_index` value
    // pristine on the CP record matches the engine's own storage model and lets the
    // resolver layer the shift on top of explicit author intent without double-shifting.
    pSys.SetCpStartShift(cp_start_shift);
    // Apply instance override control points.  An explicit override revives a null-offset
    // slot: the author has set a real position, so the runtime should no longer treat this
    // CP as "unassigned" and must not substitute the bounded particle position.
    for (int i = 0; i < 8; i++) {
        if (over.controlpointOverrides[i].active) {
            pcs[i].offset =
                Eigen::Vector3d { array_cast<double>(over.controlpointOverrides[i].offset).data() };
            pcs[i].is_null_offset = false;
        }
        if (over.controlpointOverrides[i].anglesActive) {
            pcs[i].angles = Eigen::Vector3d {
                array_cast<double>(over.controlpointOverrides[i].angles).data()
            };
            // No runtime operator consumes CP angles yet; report the capture so a future
            // consumer (or debug session) can see the data reached the CP struct.  Per
            // feedback_no_stubs.md: "Implement the API or report the gap loudly."
            LOG_INFO("instanceoverride.controlpointangle%d captured: (%.5f, %.5f, %.5f) rad "
                     "— no runtime operator consumes CP angles yet",
                     i,
                     pcs[i].angles.x(),
                     pcs[i].angles.y(),
                     pcs[i].angles.z());
        }
    }
    // Convert worldspace control points to local space
    for (usize i = 0; i < pcs.size(); i++) {
        if (pcs[i].worldspace) {
            pcs[i].offset -= origin_vec;
        }
    }
    // Seed resolved = offset so operators that read `resolved` without a parent chain see the
    // static value immediately (matches legacy behavior for the 99.7% of CPs that never chain).
    // Per-frame resolution happens in ParticleSubSystem::ResolveControlpointsForInstance.
    for (auto& pc : pcs) {
        pc.resolved = pc.offset;
    }
}
void LoadInitializer(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                     const wpscene::ParticleInstanceoverride& over, u32 rope_count = 0,
                     int /*cp_start*/ = 0, bool is_rope = false) {
    // cp_start is intentionally unused here.  The child-container `controlpointstartindex`
    // is stored on the subsystem (see LoadControlPoint → SetCpStartShift) and applied at
    // chain-resolve time, so injecting it into per-initializer JSON would double-shift.
    for (const auto& ini : wp.initializers) {
        nlohmann::json iniCopy = ini;
        // Inject/override count for mapsequencebetweencontrolpoints
        if (rope_count > 0 && iniCopy.contains("name") &&
            iniCopy["name"] == "mapsequencebetweencontrolpoints") {
            iniCopy["count"] = rope_count;
        }
        for (int32_t cp : CollectCpReferences(iniCopy)) pSys.MarkCpReferenced(cp);
        pSys.AddInitializer(WPParticleParser::genParticleInitOp(iniCopy, pSys.Controlpoints()));
    }
    if (over.enabled) pSys.AddInitializer(WPParticleParser::genOverrideInitOp(over, is_rope));
}
void LoadOperator(ParticleSubSystem& pSys, const wpscene::Particle& wp,
                  const wpscene::ParticleInstanceoverride& over) {
    for (const auto& op : wp.operators) {
        for (int32_t cp : CollectCpReferences(op)) pSys.MarkCpReferenced(cp);
        pSys.AddOperator(WPParticleParser::genParticleOperatorOp(op, over));
    }
}
void LoadEmitter(ParticleSubSystem& pSys, const wpscene::Particle& wp, float count, float rate,
                 bool render_rope, u32 rope_batch_size = 1, bool has_periodic = false) {
    bool                                 sort      = render_rope;
    bool                                 any_audio = false;
    audio_reactive::RateMultiplierParams audioParams {};
    for (const auto& em : wp.emitters) {
        if (em.audioprocessingmode != 0 && ! any_audio) {
            // First audio-reactive emitter on this subsystem wins.  Multiple
            // emitters per subsystem with diverging audio params would need
            // per-emitter state on the subsystem — out of scope; in practice
            // wallpapers tend to set a single audio-reactive emitter.
            any_audio                    = true;
            audioParams.mode             = em.audioprocessingmode;
            audioParams.weShapeAuthored  = em.audio_we_shape_authored;
            audioParams.freqStart        = em.audioprocessingfrequencystart;
            audioParams.freqEnd          = em.audioprocessingfrequencyend;
            audioParams.boundsLow        = em.audioprocessingbounds[0];
            audioParams.boundsHigh       = em.audioprocessingbounds[1];
            audioParams.exponent         = em.audioprocessingexponent;
            audioParams.amount           = em.audioprocessing;
        }
        auto  newEm      = em;
        float burst_rate = 0.0f;
        if (rope_batch_size > 1 && ! has_periodic && count * rate > 0.001f) {
            // Rope+mapsequence without periodic: rate override controls bolt frequency.
            // Keep base emission rate, use override as burst period.
            burst_rate = 1.0f / (count * rate);
            LOG_INFO("burst emitter: period=%.3fs (rate_override=%.3f, batch=%u)",
                     burst_rate,
                     count * rate,
                     rope_batch_size);
        } else {
            // Only instanceoverride.count scales emission rate.  The `rate` field
            // in instanceoverride does NOT mean "emit-rate multiplier" in WE — when
            // we treated it that way (count*rate), Voyager's rate=5.0 inflated emit
            // rate 5x past maxcount/lifetime, producing a 1Hz population pulse.
            // (rate may control playback speed / time dilation in WE; we don't
            // implement that yet, but ignoring it is safer than misapplying it.)
            newEm.rate *= count;
        }
        pSys.AddEmitter(
            WPParticleParser::genParticleEmittOp(newEm, sort, rope_batch_size, burst_rate));
    }
    if (any_audio) {
        // SceneWallpaper's per-frame loop will sample the FFT spectrum each
        // tick and push a multiplier via SetAudioRateMultiplier; folded into
        // rate_eff in ParticleSubSystem::Emitt.
        pSys.MarkAudioReactive();
        pSys.SetAudioParams(audioParams);
        LOG_INFO("particle subsystem audio-reactive: mode=%u, freq=[%d,%d), bounds=[%.2f,%.2f], "
                 "exp=%.2f, amount=%.2f, we_shape=%d",
                 audioParams.mode,
                 audioParams.freqStart,
                 audioParams.freqEnd,
                 audioParams.boundsLow,
                 audioParams.boundsHigh,
                 audioParams.exponent,
                 audioParams.amount,
                 audioParams.weShapeAuthored ? 1 : 0);
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

    // Workaround: chromatic_aberration's AUDIOPROCESSING=0 branch uses a
    // `timer` vec4 (a COLOR sampled from the input texture) as the UV-offset
    // multiplier.  WE's runtime apparently cooperates with that, but our
    // glslang + FixImplicitConversions path produces sampling that pulls the
    // text off the canvas on stacked CA passes (see wallpaper 2866203962
    // id=210 VHS time/date text — eff[1] uses default AUDIOPROCESSING=0).
    //
    // Bump AUDIOPROCESSING to 3 (stereo audio response) when unset or 0 for
    // any workshop/2423877731 chromatic_aberration material.  The audio-
    // reactive branch is numerically well-behaved (we already clamp the
    // shift via ClampAudioReactiveShift) and visually matches WE closely.
    if (wpmat.shader.find("chromatic_aberration") != std::string::npos) {
        auto it = pWPShaderInfo->combos.find("AUDIOPROCESSING");
        if (it == pWPShaderInfo->combos.end() || it->second == "0") {
            pWPShaderInfo->combos["AUDIOPROCESSING"] = "3";
            static bool s_logged_ca                  = false;
            if (! s_logged_ca) {
                s_logged_ca = true;
                LOG_INFO("chromatic_aberration: forced AUDIOPROCESSING=3 "
                         "(default 0 produces text-shift artifacts via the "
                         "timer+pointer UV-offset path).  First occurrence — "
                         "not logged on subsequent materials.");
            }
        }
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
            // Not a renderer error — WE editor often lets authors expose
            // material settings in the UI that their custom shader never
            // actually declares as uniforms.  Solar system (3662790108) floods
            // the journal with ~984 "Color"/"Alpha" misses per load, plus
            // per-planet "Shadow Strength|阴影强度" etc. from `dqss2` effect,
            // because none of those uniforms exist in the compiled GLSL.
            // Keep it at INFO so a future shader-parser gap still surfaces
            // (searchable via `ShaderValue:`) without drowning real errors.
            LOG_INFO("ShaderValue: '%s' has no matching GLSL uniform (author default, skipped)",
                     name.c_str());
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
            reflCam->SetFadeEnabled(sc.general.camerafade);
            scene.cameras["reflected_perspective"] = reflCam;

            scene.activeCamera->LoadPaths(std::move(camPaths));
            scene.activeCamera->SetFadeEnabled(sc.general.camerafade);
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

        // Enable 4x MSAA for 2D scenes too — warp-streak particles and
        // similar thin additive quads alias badly at 1spp (blocky edges).
        scene.msaaSamples = 4;
        LOG_INFO("MSAA enabled: x%d (2D scene)", scene.msaaSamples);

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

        // Store mutable copies on Scene for runtime SceneScript control
        context.scene->ambientColor  = sc.general.ambientcolor;
        context.scene->skylightColor = sc.general.skylightcolor;
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

    // Autosize resolution: when the image-model JSON declared "autosize": true
    // and neither the model nor the scene object provided explicit dimensions,
    // derive size from the first texture's sprite-frame metadata (or the
    // texture's mapWidth/mapHeight for non-sprite pictures).  WPImageObject's
    // FromJson intentionally leaves size at default (2,2) in that case so we
    // can resolve here with the texture parser in scope.
    if (wpimgobj.autosize && context.scene && context.scene->imageParser &&
        ! wpimgobj.material.textures.empty()) {
        const auto& texName = wpimgobj.material.textures.front();
        if (! texName.empty()) {
            auto                 header = context.scene->imageParser->ParseHeader(texName);
            std::array<float, 2> resolved { 0.0f, 0.0f };
            if (header.isSprite && header.spriteAnim.numFrames() > 0) {
                const auto& frame = header.spriteAnim.GetCurFrame();
                resolved          = { frame.width, frame.height };
            } else if (header.mapWidth > 0 && header.mapHeight > 0) {
                resolved = { (float)header.mapWidth, (float)header.mapHeight };
            }
            if (resolved[0] > 0.0f && resolved[1] > 0.0f) {
                LOG_INFO("  autosize id=%d name='%s' tex='%s' resolved=(%.1f,%.1f)",
                         wpimgobj.id,
                         wpimgobj.name.c_str(),
                         texName.c_str(),
                         resolved[0],
                         resolved[1]);
                wpimgobj.size = resolved;
            }
        }
    }

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
    // Layers whose visibility can flip at runtime need to stay in the main
    // render graph: combo-selectors (character picker user props) and layers
    // referenced by SceneScript via getLayer('name').  Everything else that
    // starts invisible gets routed to an offscreen RT so compose layers can
    // still reference its texture.
    bool isScriptedLayer =
        ! wpimgobj.name.empty() && context.script_referenced_layers.count(wpimgobj.name) > 0;
    bool isOffscreen = ! wpimgobj.visible && ! wpimgobj.visibleIsComboSelector && ! isScriptedLayer;

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
    // No-effect fullscreen layer: skip rendering BUT still register a bare
    // placeholder node so SceneScript can find this layer via
    // thisScene.getLayer(name).  Wallpapers (e.g. 3470764447 Nightingale "后处理层")
    // use fullscreen layers as script hosts — the layer's display script reads
    // engine.canvasSize, calls getLayer('morning')/getLayer('day')/etc, and
    // toggles their visibility based on time of day.  Without the placeholder,
    // every script-host fullscreen layer logs `getLayer: unknown layer: <name>`
    // when its property scripts initialize `thisLayer = thisScene.getLayer(...)`,
    // and any script that touches `thisLayer.foo` would crash quietly.
    if (! hasEffect && wpimgobj.fullscreen) {
        auto spPlaceholder = std::make_shared<SceneNode>(Vector3f(wpimgobj.origin.data()),
                                                         Vector3f(wpimgobj.scale.data()),
                                                         Vector3f(wpimgobj.angles.data()));
        spPlaceholder->ID() = wpimgobj.id;
        spPlaceholder->SetVisible(wpimgobj.visible);
        spPlaceholder->SetOffscreen(true); // never rendered (no mesh/material)
        if (wpimgobj.parent_id >= 0 && context.node_map.count(wpimgobj.parent_id)) {
            context.node_map.at(wpimgobj.parent_id)->AppendChild(spPlaceholder);
        } else {
            context.scene->sceneGraph->AppendChild(spPlaceholder);
        }
        context.node_map[wpimgobj.id] = spPlaceholder;
        LOG_INFO("fullscreen no-effect layer id=%d name='%s': registered as script-host "
                 "placeholder (no rendering)",
                 wpimgobj.id,
                 wpimgobj.name.c_str());
        return;
    }

    bool hasPuppet = ! wpimgobj.puppet.empty();
    (void)hasPuppet;

    bool isCompose = (wpimgobj.image == "models/util/composelayer.json");
    // A no-effect compose layer still needs an output RT so dependent nodes can
    // reference `_rt_imageLayerComposite_<id>_a` via their `dependencies` list
    // and `textures` slots.  Synthesize an effectpassthrough so the compose
    // layer runs its normal offscreen path and produces the expected RT.
    if (! hasEffect && isCompose) {
        wpscene::WPImageEffect passEffect;
        wpscene::WPMaterial    passMat;
        nlohmann::json         json;
        if (PARSE_JSON(fs::GetFileContent(vfs, "/assets/materials/util/effectpassthrough.json"),
                       json)) {
            passMat.FromJson(json);
            passMat.combos["BONECOUNT"] = 1;
            passMat.blending            = "normal";
            passEffect.materials.push_back(std::move(passMat));
            wpimgobj.effects.push_back(std::move(passEffect));
            count_eff = 1;
            hasEffect = true;
            LOG_INFO("  compose layer id=%d has no effect — synthesized effectpassthrough "
                     "for dependent link RT",
                     wpimgobj.id);
        } else {
            LOG_ERROR("compose layer id=%d: failed to load effectpassthrough.json", wpimgobj.id);
            return;
        }
    }

    std::unique_ptr<WPMdl> puppet;
    if (! wpimgobj.puppet.empty()) {
        puppet = std::make_unique<WPMdl>();
        if (! WPMdlParser::Parse(wpimgobj.puppet, vfs, *puppet)) {
            LOG_ERROR("parse puppet failed: %s", wpimgobj.puppet.c_str());
            puppet = nullptr;
        } else if (! puppet->is_puppet || ! puppet->puppet) {
            // scene.json declared this file under "puppet:" but the MDL parsed
            // cleanly as a non-puppet mesh (flag 9/11/15/39): WPMdlParser's
            // non-puppet branch never instantiates the WPPuppet shared_ptr.
            // Fall back to the regular image-quad path instead of dereferencing
            // the null bones vector below.
            LOG_INFO("non-puppet MDL referenced as puppet, falling back to image quad: %s",
                     wpimgobj.puppet.c_str());
            puppet = nullptr;
        } else if (puppet->puppet->bones.size() == 0) {
            LOG_ERROR("puppet has no bones: %s", wpimgobj.puppet.c_str());
            puppet = nullptr;
        }
    }

    // Diagnostic: capture MDL vertex bounds + scene.json size so a child's
    // attachment-resolution log can compare attMat coordinates against the
    // parent's MDL space and scene size.  Helps identify which coordinate
    // space MDAT attachment matrices live in (MDL-pixel vs scene-size).
    if (puppet && ! puppet->vertexs.empty()) {
        float xmin = std::numeric_limits<float>::infinity();
        float ymin = std::numeric_limits<float>::infinity();
        float zmin = std::numeric_limits<float>::infinity();
        float xmax = -std::numeric_limits<float>::infinity();
        float ymax = -std::numeric_limits<float>::infinity();
        float zmax = -std::numeric_limits<float>::infinity();
        for (const auto& v : puppet->vertexs) {
            xmin = std::min(xmin, v.position[0]);
            ymin = std::min(ymin, v.position[1]);
            zmin = std::min(zmin, v.position[2]);
            xmax = std::max(xmax, v.position[0]);
            ymax = std::max(ymax, v.position[1]);
            zmax = std::max(zmax, v.position[2]);
        }
        context.node_mdl_bounds[wpimgobj.id]   = { xmin, ymin, zmin, xmax, ymax, zmax };
        context.node_scene_size[wpimgobj.id]   = { wpimgobj.size[0], wpimgobj.size[1] };
        context.node_name_for_log[wpimgobj.id] = wpimgobj.name;
        LOG_INFO("MDL bounds id=%d name='%s' x=[%.1f,%.1f] y=[%.1f,%.1f] span=(%.0f,%.0f) "
                 "scene_size=(%.0f,%.0f) ratio=(%.2f,%.2f) attachments=%zu",
                 wpimgobj.id,
                 wpimgobj.name.c_str(),
                 xmin,
                 xmax,
                 ymin,
                 ymax,
                 xmax - xmin,
                 ymax - ymin,
                 wpimgobj.size[0],
                 wpimgobj.size[1],
                 (xmax - xmin) > 0 ? wpimgobj.size[0] / (xmax - xmin) : 0.0f,
                 (ymax - ymin) > 0 ? wpimgobj.size[1] / (ymax - ymin) : 0.0f,
                 puppet->puppet ? puppet->puppet->attachments.size() : 0u);
        if (puppet->puppet) {
            for (const auto& att : puppet->puppet->attachments) {
                const auto& m = att.transform.matrix();
                float       nx = (xmax - xmin) > 0 ? (m(0, 3) - xmin) / (xmax - xmin) : 0.0f;
                float       ny = (ymax - ymin) > 0 ? (m(1, 3) - ymin) / (ymax - ymin) : 0.0f;
                LOG_INFO("  ATT '%s' trans=(%.2f, %.2f, %.2f) bone[0]_trans=(%.2f, %.2f) "
                         "norm_in_mdl=(%.0f%%, %.0f%%)",
                         att.name.c_str(),
                         m(0, 3),
                         m(1, 3),
                         m(2, 3),
                         puppet->puppet->bones.empty() ? 0.0f
                                                       : puppet->puppet->bones[0].transform.matrix()(0, 3),
                         puppet->puppet->bones.empty() ? 0.0f
                                                       : puppet->puppet->bones[0].transform.matrix()(1, 3),
                         nx * 100.0f,
                         ny * 100.0f);
            }
        }
    }

    // wpimgobj.origin[1] = context.ortho_h - wpimgobj.origin[1];
    auto spImgNode = std::make_shared<SceneNode>(Vector3f(wpimgobj.origin.data()),
                                                 Vector3f(wpimgobj.scale.data()),
                                                 Vector3f(wpimgobj.angles.data()));
    LoadAlignment(*spImgNode, wpimgobj.alignment, { wpimgobj.size[0], wpimgobj.size[1] });
    spImgNode->ID() = wpimgobj.id;
    // Copy parsed property animations (alpha.animation etc.) into the scene
    // registry so they can be ticked by the render thread and controlled via
    // SceneScript's layer.getAnimation(name).  Apply startPaused: WE plays by
    // default unless the author marks it paused.
    if (! wpimgobj.propertyAnimations.empty()) {
        auto& vec = context.scene->nodePropertyAnimations[wpimgobj.id];
        for (auto panim : wpimgobj.propertyAnimations) {
            panim.playing = ! panim.startPaused;
            panim.time    = 0.0;
            LOG_INFO("register prop anim: node=%d prop=%s name='%s' playing=%d",
                     wpimgobj.id,
                     panim.property.c_str(),
                     panim.name.c_str(),
                     (int)panim.playing);
            vec.push_back(std::move(panim));
        }
    }
    // Only set m_visible for combo-selector nodes — they need runtime toggling.
    // Other invisible nodes keep m_visible=true; isOffscreen controls rendering.
    // Setting m_visible=false would cascade to effect chain nodes via
    // m_visibilityOwner, preventing them from rendering to their offscreen RTs.
    //
    // Scripted layers are the exception — they're kept in the main render
    // graph so SetVisible() can toggle them at runtime.  Initialize their
    // m_visible to match the JSON so they start in the correct state.
    if (wpimgobj.visibleIsComboSelector || isScriptedLayer) {
        spImgNode->SetVisible(wpimgobj.visible);
    }
    // Debug hide-pattern filter.  When set (via SceneWallpaper::setHidePattern,
    // wired from sceneviewer's --hide-pattern CLI flag), hides any scene
    // object whose name contains any comma-separated needle.  Useful for
    // isolating which cards occupy which world positions when a cluster of
    // same-attachment siblings overlaps.
    if (! context.hide_pattern.empty()) {
        auto to_lower = [](std::string_view s) {
            std::string r(s);
            for (auto& c : r) c = (char)std::tolower((unsigned char)c);
            return r;
        };
        const std::string name_lower = to_lower(wpimgobj.name);
        std::string_view  needles(context.hide_pattern);
        while (! needles.empty()) {
            auto        comma  = needles.find(',');
            auto        needle = needles.substr(0, comma);
            std::string needle_lower = to_lower(needle);
            if (! needle_lower.empty()
                && name_lower.find(needle_lower) != std::string::npos) {
                spImgNode->SetVisible(false);
                context.hidden_names.push_back(wpimgobj.name + " (id=" +
                                               std::to_string(wpimgobj.id) + ")");
                LOG_INFO("HidePattern: id=%d name='%s' (match '%s')",
                         wpimgobj.id,
                         wpimgobj.name.c_str(),
                         needle_lower.c_str());
                break;
            }
            if (comma == std::string_view::npos) break;
            needles.remove_prefix(comma + 1);
        }
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
        // WE flat.frag reads g_Color (vec3) and g_Alpha (float) as separate
        // uniforms — distinct from the g_Color4 vec4 used by image shaders.
        // Without explicit values, GLSL leaves them zero-initialized which
        // happens to match WE's solidlayer convention (transparent placeholder)
        // but breaks every other use of `flat` (e.g. shape-quads with authored
        // color).  Populate both unconditionally; for solidlayer placeholders
        // override g_Alpha=0 so the per-image effect chain reads a clean
        // (0,0,0,0) base instead of an opaque colored quad.
        baseConstSvs["g_Color"] = std::array<float, 3> {
            wpimgobj.color[0], wpimgobj.color[1], wpimgobj.color[2]
        };
        baseConstSvs["g_Alpha"] = wpimgobj.solidlayer ? 0.0f : wpimgobj.alpha;

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
            // Not a renderer error — WE editor often lets authors expose
            // material settings in the UI that their custom shader never
            // actually declares as uniforms.  Solar system (3662790108) floods
            // the journal with ~984 "Color"/"Alpha" misses per load, plus
            // per-planet "Shadow Strength|阴影强度" etc. from `dqss2` effect,
            // because none of those uniforms exist in the compiled GLSL.
            // Keep it at INFO so a future shader-parser gap still surfaces
            // (searchable via `ShaderValue:`) without drowning real errors.
            LOG_INFO("ShaderValue: '%s' has no matching GLSL uniform (author default, skipped)",
                     name.c_str());
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
    //
    // The node's OWN world transform chains through the parent's original
    // world (no bone offset) — this node renders itself at the mesh center.
    //
    // A separate "child-attachment" transform is computed for puppet objects:
    // self.child_attachment = self.original * self.puppet.bone[0].transform.
    // Children of this node use it as their anchor so hair/body pieces latch
    // onto the puppet's root bone (head) instead of the mesh center.  Bone
    // offsets do NOT compound through the chain — only the direct puppet
    // parent's bone[0] applies to a given child.
    {
        Eigen::Matrix4d local = spImgNode->GetLocalTrans();
        // Track this node's puppet so its children can resolve their
        // "attachment" field against this puppet's MDAT attachment table.
        if (puppet && puppet->puppet) {
            context.node_puppet[wpimgobj.id] = puppet->puppet;
        }

        if (wpimgobj.parent_id >= 0) {
            // scene.json stores each child's "attachment" = name of an MDAT
            // attachment point in the PARENT puppet's skeleton (e.g. "head",
            // "hair back", "Attachment").  If the parent has a puppet with a
            // matching attachment, anchor this child to that attachment's
            // transform — that's how WE rigs hair to head, body to pelvis, etc.
            // Otherwise fall back to the parent's mesh center.
            Eigen::Matrix4d parent_chain = Eigen::Matrix4d::Identity();
            if (context.original_world_transforms.count(wpimgobj.parent_id)) {
                parent_chain = context.original_world_transforms[wpimgobj.parent_id];
            }
            // Resolve parent's puppet (may be null for non-puppet parents
            // or unknown parent ids).  Used both for diagnostic logging
            // below and for the actual composition via composeAttachedChildWorld.
            std::shared_ptr<WPPuppet> parent_puppet;
            {
                auto pit = context.node_puppet.find(wpimgobj.parent_id);
                if (pit != context.node_puppet.end()) parent_puppet = pit->second;
            }

            // Diagnostic: dump where the named attachment's transform lives
            // in the parent's MDL bounds vs the parent's scene.json size.
            // The "scaled" line shows what the offset would be if the
            // attachment translation were proportionally remapped from MDL
            // pixel units to scene-size units.  No effect on rendering;
            // grep for "ATT " in the sceneviewer log.
            if (parent_puppet && ! wpimgobj.attachment.empty()) {
                if (auto* att = parent_puppet->findAttachment(wpimgobj.attachment)) {
                    if (att->bone_index < parent_puppet->bones.size()) {
                        Eigen::Matrix4d attMat =
                            att->transform.matrix().cast<double>();
                        auto bit = context.node_mdl_bounds.find(wpimgobj.parent_id);
                        auto sit = context.node_scene_size.find(wpimgobj.parent_id);
                        auto nit = context.node_name_for_log.find(wpimgobj.parent_id);
                        const char* parent_name =
                            (nit != context.node_name_for_log.end()) ? nit->second.c_str() : "?";
                        double tx = attMat(0, 3);
                        double ty = attMat(1, 3);
                        double tz = attMat(2, 3);
                        double xmin = 0, ymin = 0, span_x = 1, span_y = 1;
                        bool   have_bounds = bit != context.node_mdl_bounds.end();
                        bool   have_size   = sit != context.node_scene_size.end();
                        if (have_bounds) {
                            xmin   = bit->second[0];
                            ymin   = bit->second[1];
                            span_x = bit->second[3] - bit->second[0];
                            span_y = bit->second[4] - bit->second[1];
                        }
                        if (have_bounds && have_size && span_x > 0 && span_y > 0) {
                            double ratio_x = sit->second[0] / span_x;
                            double ratio_y = sit->second[1] / span_y;
                            LOG_INFO(
                                "ATT child id=%d name='%s' attach='%s' parent_id=%d ('%s')",
                                wpimgobj.id,
                                wpimgobj.name.c_str(),
                                wpimgobj.attachment.c_str(),
                                wpimgobj.parent_id,
                                parent_name);
                            LOG_INFO("  attMat raw trans=(%.2f, %.2f, %.2f)", tx, ty, tz);
                            LOG_INFO("  parent mdl_span=(%.0f,%.0f) scene_size=(%.0f,%.0f) "
                                     "ratio=(%.2f,%.2f)",
                                     span_x,
                                     span_y,
                                     sit->second[0],
                                     sit->second[1],
                                     ratio_x,
                                     ratio_y);
                            LOG_INFO("  attMat if scaled to scene-size = (%.2f, %.2f)",
                                     tx * ratio_x,
                                     ty * ratio_y);
                            LOG_INFO("  norm_in_parent_mdl = (%.0f%%, %.0f%%)",
                                     span_x > 0 ? (tx - xmin) / span_x * 100.0 : 0.0,
                                     span_y > 0 ? (ty - ymin) / span_y * 100.0 : 0.0);
                            LOG_INFO("  parent_world=(%.1f,%.1f) -> with raw attMat: (%.1f,%.1f)",
                                     parent_chain(0, 3),
                                     parent_chain(1, 3),
                                     parent_chain(0, 3) + tx,
                                     parent_chain(1, 3) + ty);
                        } else {
                            LOG_INFO("ATT child id=%d name='%s' attach='%s' parent_id=%d "
                                     "(no parent bounds/size cached)",
                                     wpimgobj.id,
                                     wpimgobj.name.c_str(),
                                     wpimgobj.attachment.c_str(),
                                     wpimgobj.parent_id);
                            LOG_INFO("  attMat raw trans=(%.2f, %.2f, %.2f)", tx, ty, tz);
                        }
                    }
                }
            }

            auto compose_result = composeAttachedChildWorld(
                parent_chain, parent_puppet, wpimgobj.attachment, local);
            context.original_world_transforms[wpimgobj.id] = compose_result.world;
        } else {
            context.original_world_transforms[wpimgobj.id] = local;
        }

        // POS dump: log final world (x,y), name, parent, attachment, local
        // origin, size.  Grep for "POS " in the sceneviewer log.
        {
            const auto& w = context.original_world_transforms[wpimgobj.id];
            LOG_INFO("POS id=%d name='%s' world=(%.1f,%.1f) parent=%d attach='%s' "
                     "local=(%.1f,%.1f) size=(%.0f,%.0f) autosize=%d puppet='%s'",
                     wpimgobj.id,
                     wpimgobj.name.c_str(),
                     (float)w(0, 3),
                     (float)w(1, 3),
                     wpimgobj.parent_id,
                     wpimgobj.attachment.c_str(),
                     wpimgobj.origin[0],
                     wpimgobj.origin[1],
                     wpimgobj.size[0],
                     wpimgobj.size[1],
                     (int)wpimgobj.autosize,
                     wpimgobj.puppet.c_str());
        }

        // Legacy child_attachment_transforms — kept for children that don't
        // name a specific attachment (falls back to bone[0]).  Will likely
        // become unused once all children rely on the scene.json
        // "attachment" field.
        Eigen::Matrix4d attach = context.original_world_transforms[wpimgobj.id];
        if (puppet && puppet->puppet && ! puppet->puppet->bones.empty()) {
            attach = attach * puppet->puppet->bones[0].transform.matrix().cast<double>();
        }
        context.child_attachment_transforms[wpimgobj.id] = attach;
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
            // Compose layer with copybackground:false — author intent is "do
            // NOT capture _rt_default into the pingpong" (e.g. Nightingale
            // 3470764447 id=249 with colorBlendMode=21 BlendReflect, where
            // capturing the screen amplifies the day-character's blue TV-glow
            // into a hard-edged blue rectangle at the layer's bbox).  Force
            // passthrough so SceneToRenderGraph takes the alternate path that
            // (a) skips the composelayer.frag base-pass screen capture, and
            // (b) honors copybackground=false to also skip the implicit copy.
            // The pingpong stays cleared, BlendReflect over a transparent
            // pingpong = no-op, no blue box.
            const bool forcePassthroughForCopyBg =
                isCompose && ! wpimgobj.copybackground;
            imgEffectLayer->SetPassthrough(isCompose &&
                                           (wpimgobj.config.passthrough || forcePassthroughForCopyBg));
            imgEffectLayer->SetCopyBackground(wpimgobj.copybackground);
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
                // Resolve the attachment anchor the same way original_world_
                // transforms does above: start from the parent's mesh center,
                // then apply the named MDAT attachment from the parent's
                // puppet if one is present.
                Eigen::Matrix4d parent_chain =
                    context.original_world_transforms[wpimgobj.parent_id];
                if (! wpimgobj.attachment.empty()) {
                    auto pit = context.node_puppet.find(wpimgobj.parent_id);
                    if (pit != context.node_puppet.end() && pit->second) {
                        if (auto* att = pit->second->findAttachment(wpimgobj.attachment)) {
                            parent_chain = parent_chain * att->transform.matrix().cast<double>();
                        }
                    }
                }
                proxy->SetWorldTransform(parent_chain);
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
                imgEffect->name = wpeffobj.name;
                imgEffectLayer->AddEffect(imgEffect);
                LOG_INFO("  effect[%d] '%s' loaded OK (%zu nodes)",
                         i_eff,
                         wpeffobj.name.c_str(),
                         imgEffect->nodes.size());
            } else {
                LOG_ERROR("effect \'%s\' failed to load", wpeffobj.name.c_str());
            }
        }
        // Store effect names per layer for SceneScript getEffect()
        {
            std::vector<std::string> effNames;
            for (size_t i = 0; i < imgEffectLayer->EffectCount(); i++) {
                effNames.push_back(imgEffectLayer->GetEffect(i)->name);
            }
            if (! effNames.empty()) {
                context.scene->layerEffectNames[wpimgobj.name] = std::move(effNames);
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
        // Disconnect parent-chain transform for nodes whose base pass uses a
        // per-node effect camera at origin:
        //   - Offscreen nodes render to dedicated RTs with the "effect" camera.
        //   - Non-compose effect images render their base pass to pingpong_a
        //     with a per-image ortho camera attached to effect_camera_node
        //     (at origin).  Leaving the parent chain attached would place
        //     the image's world position outside [-1,1] NDC (e.g. characters
        //     positioned at scene center (1920,1080) when camera ortho is
        //     sized to the image), clipping all geometry.  CopyTrans(identity)
        //     earlier reset the local transform; clearing the parent here
        //     completes the reset so ModelTrans = identity for this pass.
        // Compose layers keep the parent chain — their base pass renders to
        // the scene-sized compose RT via the scene ortho camera, so world
        // position is correct there.
        bool disconnect_parent = isOffscreen || (hasEffect && ! isCompose);
        if (disconnect_parent) {
            spImgNode->InheritParent(SceneNode());
        }
        LOG_INFO("  ParseImageObj id=%d completed, added as child of parent %d (parent_cleared=%d)",
                 wpimgobj.id,
                 wpimgobj.parent_id,
                 (int)disconnect_parent);
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
        // Visibility policy mirrors the image-object logic: only honor
        // JSON `visible: false` when the object is a *managed* layer —
        //   (a) a SceneScript-referenced name (getLayer(name)), or
        //   (b) a pool layer (registered via registerAsset + createLayer).
        // Both cases are tracked in context.script_referenced_layers
        // (populated at scan time in the SceneScript literal pass and at
        // pool-synthesis time).
        //
        // For ambient particle objects with no script reference — e.g. the
        // NieR:Automata 2B halo cluster (Вихрь 2 + 3× Искра at her head, all
        // hardcoded `"visible": false` with no script to enable them) — WE's
        // runtime apparently treats `visible: false` as an editor-time-only
        // hint and renders them anyway.  Match that: start them visible.
        bool isScriptedParticle = ! wppartobj.name.empty() &&
                                  context.script_referenced_layers.count(wppartobj.name) > 0;
        if (isScriptedParticle) {
            spNode->SetVisible(wppartobj.visible);
        }
        // Ambient particles: leave m_visible at the ctor default (true).
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
        u32 maxcount = std::min(particle_obj.maxcount, 20000u);
        // instanceoverride.rate = time-dilation factor: system clock runs at
        // 1/rate of scene time (rate=5.0 → 5x slower).  Applied to the subsystem
        // as m_rate, it scales particleTime, which naturally slows emit timing,
        // lifetime decrement, and movement operators in lockstep.
        const double time_scale =
            override.enabled && override.rate > 0.0f ? 1.0 / (double) override.rate : 1.0;
        auto spMesh      = std::make_shared<SceneMesh>(true);
        auto particleSub = std::make_unique<ParticleSubSystem>(
            *context.scene->paritileSys,
            spMesh,
            maxcount,
            time_scale,
            child_data.maxcount,
            child_data.probability,
            ParseSpawnType(child_data.type),
            [](const Particle&, const ParticleRawGenSpec&) {
            },
            particle_obj.starttime);

        // Debug name: basename of the particle preset.  Used for per-subsystem logs in
        // ParticleSystem.cpp (particle-clear / particle-state lines).
        {
            std::string dn = is_child ? child_ptr.child->name : wppartobj.name;
            auto slash = dn.find_last_of('/');
            if (slash != std::string::npos) dn = dn.substr(slash + 1);
            auto dot = dn.find_last_of('.');
            if (dot != std::string::npos) dn = dn.substr(0, dot);
            particleSub->SetDebugName(std::move(dn));
        }

        LoadControlPoint(*particleSub,
                         particle_obj,
                         override,
                         wppartobj.origin,
                         child_data.controlpointstartindex);
        LoadEmitter(*particleSub, particle_obj, override.count, override.rate, false, 1);
        LoadInitializer(*particleSub, particle_obj, override, 0, child_data.controlpointstartindex);
        LoadOperator(*particleSub, particle_obj, override);

        for (auto& child : particle_obj.children) {
            if (const char* skip_list = std::getenv("WEKDE_SKIP_PARTICLE_CHILD_SUBSTR")) {
                bool        matched = false;
                std::string list(skip_list);
                size_t      start = 0;
                while (start < list.size()) {
                    size_t comma = list.find(',', start);
                    if (comma == std::string::npos) comma = list.size();
                    std::string tok = list.substr(start, comma - start);
                    if (! tok.empty() && child.name.find(tok) != std::string::npos) {
                        matched = true;
                        LOG_INFO("WEKDE_SKIP_PARTICLE_CHILD_SUBSTR: skipping child '%s' (match '%s')",
                                 child.name.c_str(),
                                 tok.c_str());
                        break;
                    }
                    start = comma + 1;
                }
                if (matched) continue;
            }
            ParseParticleObj(context,
                             wppartobj,
                             { .child             = &child,
                               .node_parent       = spNode.get(),
                               .particle_parent   = particleSub.get(),
                               .max_instancecount = child_ptr.max_instancecount });
        }

        if (is_child)
            child_ptr.particle_parent->AddChild(std::move(particleSub));
        else {
            // Pool particle nodes get registered so createLayer can reset
            // them and auto-hide when their burst FX plays out.  Non-pool
            // particles (regular wallpaper emitters) must NOT be in this
            // map — a continuous emitter could momentarily have zero live
            // particles and get wrongly flagged as "done".
            if (wppartobj.name.rfind("__pool_", 0) == 0) {
                context.scene->particleSubByNodeId[wppartobj.id] = particleSub.get();
            }
            context.scene->paritileSys->subsystems.emplace_back(std::move(particleSub));
        }

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

    // instanceoverride.count scales the effective particle count.  LoadEmitter
    // also scales emission rate by the same factor so capacity / emit-rate
    // stays balanced (otherwise emit_rate*lifetime exceeds maxcount and the
    // population cycles visibly at 1Hz — the Voyager starfield bug).
    const float count_factor = override.enabled ? std::max(override.count, 0.001f) : 1.0f;
    u32         maxcount     = (u32)std::ceil(particle_obj.maxcount * count_factor);
    maxcount                 = std::min(maxcount, 20000u);

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
        shaderInfo.combos["THICKFORMAT"] = "1";
        // TRAILRENDERER selects the trail-history UV math in genericropeparticle.vert.
        // Plain rope (mapsequencebetweencontrolpoints) is a static ladder between
        // two CPs, not a history trail — its VS path is the `#else` branch.
        // Setting TRAILRENDERER for plain rope wedges the first segment to
        // `uvMinimum=0, uvDelta=0`, which samples the texture's V=0 edge across
        // the whole quad — a transparent strip on beam textures, killing
        // visibility.  Policy mirrors WE's binary dispatch — see WPRopeCombos.hpp.
        if (RendererSetsTrailRendererCombo(wppartRenderer.name))
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

    // instanceoverride.rate = time-dilation factor (rate=5.0 → 5x slower).  See
    // the spawner-only branch above for detailed rationale.
    // instanceoverride.rate = playback-speed multiplier: rate=5.0 → system
    // runs 5× faster (emit timing, lifetime decrement, movement all accelerate).
    // We apply it to the subsystem's m_rate, which multiplies particleTime
    // passed to the emitter + operators.
    const double time_scale =
        override.enabled && override.rate > 0.0f ? (double) override.rate : 1.0;
    auto particleSub = std::make_unique<ParticleSubSystem>(
        *context.scene->paritileSys,
        spMesh,
        maxcount,
        time_scale,
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

    {
        std::string dn = is_child ? child_ptr.child->name : wppartobj.name;
        auto slash = dn.find_last_of('/');
        if (slash != std::string::npos) dn = dn.substr(slash + 1);
        auto dot = dn.find_last_of('.');
        if (dot != std::string::npos) dn = dn.substr(0, dot);
        particleSub->SetDebugName(std::move(dn));
    }

    LoadControlPoint(
        *particleSub, particle_obj, override, wppartobj.origin, child_data.controlpointstartindex);

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
    LoadInitializer(*particleSub,
                    particle_obj,
                    override,
                    rope_init_count,
                    child_data.controlpointstartindex,
                    /*is_rope=*/render_rope);
    LoadOperator(*particleSub, particle_obj, override);

    mesh.AddMaterial(std::move(material));
    spNode->AddMesh(spMesh);
    context.shader_updater->SetNodeData(spNode.get(), svData);

    for (auto& child : particle_obj.children) {
        // Debug knob: WEKDE_SKIP_PARTICLE_CHILD_SUBSTR=foo,bar skips any child
        // whose preset name contains one of the comma-separated substrings.
        // Useful for isolating visual contributions of nested particle sets
        // (e.g. WEKDE_SKIP_PARTICLE_CHILD_SUBSTR=beam_child to turn off the
        // NieR 2B thunderbolt sub-beams without touching the workshop asset).
        if (const char* skip_list = std::getenv("WEKDE_SKIP_PARTICLE_CHILD_SUBSTR")) {
            bool        matched = false;
            std::string list(skip_list);
            size_t      start = 0;
            while (start < list.size()) {
                size_t comma = list.find(',', start);
                if (comma == std::string::npos) comma = list.size();
                std::string tok = list.substr(start, comma - start);
                if (! tok.empty() && child.name.find(tok) != std::string::npos) {
                    matched = true;
                    LOG_INFO("WEKDE_SKIP_PARTICLE_CHILD_SUBSTR: skipping child '%s' (match '%s')",
                             child.name.c_str(),
                             tok.c_str());
                    break;
                }
                start = comma + 1;
            }
            if (matched) continue;
        }
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
    else {
        // Pool-only: see comment in the spawner-only branch above
        if (wppartobj.name.rfind("__pool_", 0) == 0) {
            context.scene->particleSubByNodeId[wppartobj.id] = particleSub.get();
        }
        context.scene->paritileSys->subsystems.emplace_back(std::move(particleSub));
    }

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

    // For invisible, empty text, or system font objects, create a placeholder node
    // so SceneScript can discover the layer via getLayer(). Scripts may set text
    // dynamically or toggle visibility at runtime.
    auto createPlaceholderNode = [&]() {
        auto spNode  = std::make_shared<SceneNode>(Vector3f(textObj.origin.data()),
                                                  Vector3f(textObj.scale.data()),
                                                  Vector3f(textObj.angles.data()));
        spNode->ID() = textObj.id;
        spNode->SetVisible(textObj.visible);
        context.scene->sceneGraph->AppendChild(spNode);
        context.node_map[textObj.id] = spNode;
        LOG_INFO("  registered placeholder node id=%d for SceneScript", textObj.id);
    };

    // systemfont / missing font: no FreeType-backed rasterization possible.
    // Fall back to a placeholder node so getLayer() still works.
    if (textObj.font.empty() || textObj.font.find("systemfont") != std::string::npos) {
        LOG_INFO("  system font or missing font '%s', creating placeholder", textObj.font.c_str());
        createPlaceholderNode();
        return;
    }

    // Load font from VFS — try /assets/ prefix first (PKG assets), then bare path.
    // Log the chosen source + byte size so font-swaps / VFS priority issues are
    // visible at a glance in the journal (the parent repo asked to confirm
    // correct font selection for wallpaper 2866203962).
    std::string fontData;
    std::string fontLoadedFrom;
    if (vfs.Contains("/assets/" + textObj.font)) {
        fontData       = fs::GetFileContent(vfs, "/assets/" + textObj.font);
        fontLoadedFrom = "/assets/" + textObj.font;
    } else if (vfs.Contains("/" + textObj.font)) {
        fontData       = fs::GetFileContent(vfs, "/" + textObj.font);
        fontLoadedFrom = "/" + textObj.font;
    }
    if (fontData.empty()) {
        LOG_ERROR("  failed to load font: %s", textObj.font.c_str());
        // Still register a placeholder so SceneScript getLayer() works.
        createPlaceholderNode();
        return;
    }
    LOG_INFO("  text id=%d font loaded from %s (%zu bytes)",
             textObj.id,
             fontLoadedFrom.c_str(),
             fontData.size());

    // Resolve a sane rasterization canvas size.  Wallpapers that rely on
    // SceneScript to fill text at runtime commonly declare `size: [2, 2]`
    // (or similar placeholder) in scene.json, meaning "autosize" — the mesh
    // is meant to match the rasterized text bounds at render time.  Fall
    // back to a canvas driven by the authored `maxwidth` (soft wrap boundary)
    // and by the authored pointsize so 1–8 lines of text fit comfortably.
    const float kDpi                  = WPTextRenderer::kRasterDpiScale;
    i32         texW                  = static_cast<i32>(textObj.size[0]);
    i32         texH                  = static_cast<i32>(textObj.size[1]);
    const i32   placeholder_threshold = 8;
    const bool  autosize_canvas = (texW <= placeholder_threshold || texH <= placeholder_threshold);
    if (autosize_canvas) {
        // Matches WPTextRenderer's DPI multiplier — see WPTextRenderer.cpp
        // rationale (scene ortho targets Retina-scale 4K, not 96 DPI viewer).
        i32 px = static_cast<i32>(textObj.pointsize * 96.0f / 72.0f * kDpi + 0.5f);
        if (px < 12) px = 12;
        // Prefer maxwidth (the author's own wrapping hint); fall back to a
        // generous 2048 otherwise.  Clamp to 4096 so pathological maxwidth
        // declarations don't blow up the canvas.
        i32 mw = (textObj.maxwidth > 0.0f) ? static_cast<i32>(textObj.maxwidth * kDpi) : 0;
        if (mw <= 0) mw = 2048;
        mw   = std::min(mw, 4096);
        texW = mw;
        // Line height at our DPI: pointsize * 96/72 * DPI * ~1.4.
        // Allow up to 8 lines for long titles.
        texH = std::max(static_cast<i32>(px * 1.4f * 8), 192);
    } else {
        // Explicit canvas size from the model JSON (e.g. 2866203962 VHS
        // Time/Date = 931×153).  The author sized this as if rendered at
        // 1× DPI, but WPTextRenderer scales glyphs by kRasterDpiScale.
        // Scale the canvas by the same factor so 2-line dynamic text
        // (time + date) doesn't clip at the bottom of the texture.
        texW = static_cast<i32>(texW * kDpi);
        texH = static_cast<i32>(texH * kDpi);
    }
    if (texW <= 0 || texH <= 0) {
        texW = 512;
        texH = 128;
    }

    // Runtime text: if the declared text is empty but the layer is addressable
    // by SceneScript (via name + getLayer), still build the full plumbing —
    // mesh, material, texture, textLayers entry — using " " as the initial
    // rasterized content.  Scripts will set thisLayer.text later, and the
    // text update path (SceneWallpaper setTextUpdate → reuploadTexture) will
    // do the right thing ONLY if the tl entry exists.  Empty placeholder
    // nodes (no entry) silently drop the update.
    std::string initialText = textObj.textValue.empty() ? std::string(" ") : textObj.textValue;
    const bool  text_is_script_driven = textObj.textValue.empty();

    // Rasterize text
    auto textImage = WPTextRenderer::RenderText(fontData,
                                                textObj.pointsize,
                                                initialText,
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
    // Enable the VERSION combo path so the fragment shader applies our
    // g_Color4 (authored `color` × alpha) — without it, genericimage2 only
    // multiplies by g_Brightness/g_UserAlpha and authored colors are
    // silently dropped.  e.g. wallpaper 2866203962 VHS Time/Date text is
    // authored yellow (1,1,0) but was rendering white because VERSION was
    // not set.
    wpMat.combos["VERSION"] = 1;

    // Keep the authored scale.  Apply the WE `anchor` field (separate from
    // halign/valign — `anchor: "right"` etc.) as a translation of the
    // origin: empirical match against wallpaper 2866203962 id=402 (track
    // title, anchor=right, maxwidth=500, authored origin (2518, 1237))
    // shows WE places the TEXT HORIZONTAL CENTER at origin.x + maxwidth/2
    // (and symmetrically for "left"/"top"/"bottom").  Without this offset
    // the track title lands ~240 scene units LEFT of where WE puts it
    // under the music player icons.
    std::array<float, 3> adjOrigin = textObj.origin;
    if (autosize_canvas && textObj.maxwidth > 0.0f) {
        float halfW = textObj.maxwidth * 0.5f;
        if (textObj.anchor == "right")
            adjOrigin[0] += halfW;
        else if (textObj.anchor == "left")
            adjOrigin[0] -= halfW;
        // top/bottom would shift Y; no known wallpaper uses those yet.
    }
    auto spNode  = std::make_shared<SceneNode>(Vector3f(adjOrigin.data()),
                                              Vector3f(textObj.scale.data()),
                                              Vector3f(textObj.angles.data()));
    spNode->ID() = textObj.id;
    // Honour the scene.json `visible` field — scripts can flip this at
    // runtime via `thisLayer.visible = true` once the layer is discovered.
    spNode->SetVisible(textObj.visible);

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

    // Mesh — position it so the authored origin matches WE's anchor point.
    //
    // WE anchors text at the origin according to halign/valign:
    //   halign=left   → origin is at the text's LEFT edge
    //   halign=center → origin is at text CENTER (default)
    //   halign=right  → origin is at text's RIGHT edge
    //   valign=top    → origin is at TOP edge (Y-up: shift mesh down)
    //   valign=bottom → origin is at BOTTOM edge
    //
    // Our GenCardMesh centers the mesh on origin, which misplaces text for
    // any non-center alignment (the Cyberpunk VHS Time/Date uses left/top —
    // authored at (2510, 940) should be the top-left corner of the text so
    // text extends right+down from there into the region right of Lucy's
    // cheek).  Apply the anchor shift for ALL text layers, not just autosize.
    //
    // For autosize text, we additionally scan the rasterized image for
    // tight pixel bounds — the mesh then covers ONLY the occupied text
    // region, with UVs mapped to that sub-rect of the canvas.  Keeps the
    // authored `scale` meaningful (it scales the visible glyphs, not the
    // padded canvas).
    auto spMesh = std::make_shared<SceneMesh>();
    {
        float anchor_nx = 0.0f, anchor_ny = 0.0f;
        if (textObj.horizontalalign == "left")
            anchor_nx = -0.5f;
        else if (textObj.horizontalalign == "right")
            anchor_nx = 0.5f;
        if (textObj.verticalalign == "top")
            anchor_ny = 0.5f; // Y-up
        else if (textObj.verticalalign == "bottom")
            anchor_ny = -0.5f;

        // Default: mesh covers the full canvas, UVs 0..1.
        float meshW = (float)texW, meshH = (float)texH;
        float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
        // Center of the "content" within the canvas, as a fraction of canvas
        // size minus 0.5.  Used to shift the mesh so the anchor coincides
        // with the visible text corner (not the canvas corner).
        float content_nx = 0.0f, content_ny = 0.0f;

        if (autosize_canvas && ! textImage->slots.empty() &&
            ! textImage->slots[0].mipmaps.empty()) {
            auto& mip = textImage->slots[0].mipmaps[0];
            auto* px  = static_cast<const uint8_t*>(mip.data.get());
            if (px) {
                i32 minX = texW, minY = texH, maxX = -1, maxY = -1;
                for (i32 y = 0; y < texH; y++) {
                    for (i32 x = 0; x < texW; x++) {
                        uint8_t a = px[(y * (i32)texW + x) * 4 + 3];
                        if (a >= 16) { // ignore AA faint pixels
                            if (x < minX) minX = x;
                            if (x > maxX) maxX = x;
                            if (y < minY) minY = y;
                            if (y > maxY) maxY = y;
                        }
                    }
                }
                if (maxX >= minX && maxY >= minY) {
                    // Small padding so glow / chromatic aberration effects
                    // have texture to sample from past glyph edges.
                    i32 padY = std::max(4, (maxY - minY) / 10);
                    // Use FULL canvas X (not tight X): runtime text changes
                    // (clocks updating, track titles becoming longer) need
                    // mesh room to grow rightward from origin.  Tight-X
                    // mesh freezes width at the first-frame text and clips
                    // anything longer; keeping canvas width lets the
                    // halign=left anchor reliably place new text at the
                    // authored left edge.  Y we still crop tight since
                    // most text is single-line.
                    i32 x0 = 0;
                    i32 x1 = texW - 1;
                    i32 y0 = std::max(0, minY - padY);
                    i32 y1 = std::min(texH - 1, maxY + padY);
                    meshW  = (float)(x1 - x0 + 1);
                    meshH  = (float)(y1 - y0 + 1);
                    u0     = (float)x0 / (float)texW;
                    u1     = (float)(x1 + 1) / (float)texW;
                    v0     = (float)y0 / (float)texH;
                    v1     = (float)(y1 + 1) / (float)texH;
                    // Content center within the original canvas, expressed
                    // as fractional offset from canvas center.  Not used to
                    // shift the mesh (the tight-cropped mesh has no concept
                    // of "canvas"); kept here for potential diagnostics.
                    content_nx = ((float)(minX + maxX) * 0.5f / (float)texW) - 0.5f;
                    content_ny = -(((float)(minY + maxY) * 0.5f / (float)texH) - 0.5f);
                    LOG_INFO("  text id=%d tight %dx%d @ uv(%.3f,%.3f)-(%.3f,%.3f) "
                             "center(%+.2f,%+.2f) within %dx%d canvas",
                             textObj.id,
                             (int)meshW,
                             (int)meshH,
                             u0,
                             v0,
                             u1,
                             v1,
                             content_nx,
                             content_ny,
                             texW,
                             texH);
                }
            }
        }

        float                       dx     = -anchor_nx * meshW;
        float                       dy     = -anchor_ny * meshH;
        float                       left   = -meshW / 2.0f + dx;
        float                       right  = meshW / 2.0f + dx;
        float                       bottom = -meshH / 2.0f + dy;
        float                       top    = meshH / 2.0f + dy;
        const std::array<float, 12> pos    = {
            left, bottom, 0.0f, left, top, 0.0f, right, bottom, 0.0f, right, top, 0.0f,
        };
        const std::array<float, 8> tex = {
            u0, v1, u0, v0, u1, v1, u1, v0,
        };
        SceneVertexArray vertex({ { WE_IN_POSITION.data(), VertexType::FLOAT3 },
                                  { WE_IN_TEXCOORD.data(), VertexType::FLOAT2 } },
                                4);
        vertex.SetVertex(WE_IN_POSITION, pos);
        vertex.SetVertex(WE_IN_TEXCOORD, tex);
        spMesh->AddVertexArray(std::move(vertex));
        (void)content_nx;
        (void)content_ny;
    }
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

    // Register every text layer — not just ones with a bundled textScript —
    // so dynamic `thisLayer.text = "..."` writes from property scripts can
    // find the tl entry and re-rasterize via setTextUpdate → reuploadTexture.
    // The wallpaper 2866203962 music player paints track title/artist this
    // way; empty-text-at-parse layers used to be placeholders (no tl entry)
    // and dropped every update silently.
    {
        TextLayerInfo tli;
        tli.id                = textObj.id;
        tli.fontData          = fontData;
        tli.pointsize         = textObj.pointsize;
        tli.texWidth          = texW;
        tli.texHeight         = texH;
        tli.padding           = textObj.padding;
        tli.halign            = textObj.horizontalalign;
        tli.valign            = textObj.verticalalign;
        tli.currentText       = initialText;
        tli.textureKey        = texKey;
        tli.script            = textObj.textScript;
        tli.scriptProperties  = textObj.textScriptProperties;
        tli.pointsizeUserProp = textObj.pointsizeUserProp;
        context.scene->textLayers.push_back(std::move(tli));
        LOG_INFO("  registered text layer id=%d for script evaluation%s",
                 textObj.id,
                 textObj.textScript.empty() ? " (runtime-text only)" : "");
    }
    (void)text_is_script_driven; // reserved for future diagnostics

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
    WEK_PROFILE_SCOPE("WPSceneParser::Parse");
    // Set user properties context for the duration of parsing
    UserPropertiesScope propsScope(&userProps);

    nlohmann::json json;
    if (! PARSE_JSON(buf, json)) return nullptr;
    wpscene::WPScene sc;
    sc.FromJson(json);
    //	LOG_INFO(nlohmann::json(sc).dump(4));

    ParseContext context;
    context.hide_pattern = m_hide_pattern;

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

    // Pre-scan all embedded SceneScript sources for two things:
    //   1. getLayer('name') references — layers that the script may toggle at
    //      runtime, so we keep them in the main render graph.
    //   2. engine.registerAsset('path') references — assets the script will
    //      instantiate at runtime via thisScene.createLayer(), which need
    //      pre-allocated scene nodes (C++-side pool, see below).
    {
        static const std::regex getLayerRe(R"(getLayer\s*\(\s*['"]([^'"]+)['"]\s*\))");
        static const std::regex registerAssetRe(R"(registerAsset\s*\(\s*['"]([^'"]+)['"]\s*\))");
        // WE's real createLayer accepts an object literal with an `image:`
        // key directly — no registerAsset call required.  Extract those
        // paths so we pre-allocate a pool for them too.  Non-greedy,
        // dot-matches-all (C++ regex has ECMAScript semantics, so use
        // [\s\S] to span newlines).
        static const std::regex createLayerLiteralRe(
            R"(createLayer\s*\(\s*\{[\s\S]*?(?:image|"image"|'image')\s*:\s*['"]([^'"]+)['"])");
        // String form: createLayer('models/foo.json') — used by scripts like
        // Naruto Shippuden 2800255344's audio-spectrum bar (passes the asset
        // path directly without registerAsset).  Pre-register the path so
        // the runtime can rent pool nodes instead of returning stubs.
        static const std::regex createLayerStringRe(
            R"(createLayer\s*\(\s*['"]([^'"]+)['"]\s*\))");
        // Script-managed pool pattern: `identifier + Pool . (pop|push|length)`.
        // When present, the script expects createLayer to produce many
        // long-lived layers it manages itself — an 8-slot backend pool is
        // nowhere near enough.
        static const std::regex scriptPoolRe(R"(\w*[Pp]ool\s*\.\s*(pop|push|length))");
        // Workshop-id declaration: `__workshopId = 'NNN'` (top of every
        // workshop-imported script).  Used to resolve bare paths like
        // 'models/bar.json' to 'models/workshop/<id>/bar.json' when the bare
        // path doesn't exist on disk.
        static const std::regex workshopIdRe(
            R"(__workshopId\s*=\s*['"]([^'"]+)['"])");
        // Integer slider maxes in the same script — used as pool-size hints.
        // Matches `max: 400` but skips `max: 0.5` and `max: 1e10` (only `\d+`).
        static const std::regex                    intMaxRe(R"(max\s*:\s*(\d+)\s*[,\n])");
        std::function<void(const nlohmann::json&)> scan;
        scan = [&](const nlohmann::json& j) {
            if (j.is_string()) {
                const std::string& s = j.get_ref<const std::string&>();
                for (auto it = std::sregex_iterator(s.begin(), s.end(), getLayerRe);
                     it != std::sregex_iterator();
                     ++it) {
                    context.script_referenced_layers.insert((*it)[1].str());
                }
                for (auto it = std::sregex_iterator(s.begin(), s.end(), registerAssetRe);
                     it != std::sregex_iterator();
                     ++it) {
                    context.registered_asset_paths.insert((*it)[1].str());
                }
                // Collect paths this script creates via literal form.
                std::vector<std::string> thisScriptPaths;
                for (auto it = std::sregex_iterator(s.begin(), s.end(), createLayerLiteralRe);
                     it != std::sregex_iterator();
                     ++it) {
                    context.registered_asset_paths.insert((*it)[1].str());
                    thisScriptPaths.push_back((*it)[1].str());
                }
                // String form: createLayer('models/foo.json')
                for (auto it = std::sregex_iterator(s.begin(), s.end(), createLayerStringRe);
                     it != std::sregex_iterator();
                     ++it) {
                    context.registered_asset_paths.insert((*it)[1].str());
                    thisScriptPaths.push_back((*it)[1].str());
                }
                // Workshop-id resolution: associate this script's createLayer
                // paths with its `__workshopId = 'NNN'` declaration so we can
                // fall back to `models/workshop/<id>/<file>.json` if the bare
                // path doesn't exist on disk (Naruto Shippuden 2800255344's
                // bar.json sits under workshop/2092495494).
                {
                    std::smatch wm;
                    if (std::regex_search(s, wm, workshopIdRe)) {
                        const std::string id = wm[1].str();
                        for (const auto& p : thisScriptPaths) {
                            context.script_asset_workshop_id[p] = id;
                        }
                    }
                }
                // If this script manages its own pool, bump the backend pool
                // size hint for everything it creates.
                if (! thisScriptPaths.empty() && std::regex_search(s, scriptPoolRe)) {
                    size_t largestMax = 0;
                    for (auto it = std::sregex_iterator(s.begin(), s.end(), intMaxRe);
                         it != std::sregex_iterator();
                         ++it) {
                        try {
                            size_t v = std::stoull((*it)[1].str());
                            if (v > largestMax) largestMax = v;
                        } catch (...) {
                        }
                    }
                    // 3x safety for multi-body scenes (e.g. 3body uses
                    // trailLength × 3 bodies).  Cap at 2048 per WE's
                    // documented layer limit.
                    size_t hint = std::min<size_t>(2048, std::max<size_t>(8, largestMax * 3));
                    for (const auto& p : thisScriptPaths) {
                        auto& cur = context.asset_pool_size_hints[p];
                        if (hint > cur) cur = hint;
                    }
                }
                // createLayer in a `for (...; i < N; ...)` loop — common pattern
                // for batch-spawning bars/stars/rings (Naruto Shippuden's audio
                // spectrum needs 64 bars).  Use the loop bound as a pool-size
                // hint when no Pool.pop pattern is present.  Detect both
                // `i < N` and `i <= N` forms.
                if (! thisScriptPaths.empty()) {
                    // Locate every for-loop bound and pick the largest, capped
                    // at 2048 to match the explicit-pool path.
                    static const std::regex forBoundRe(
                        R"(for\s*\([^)]*?\b\w+\s*<=?\s*(\d+))");
                    size_t largestBound = 0;
                    for (auto it = std::sregex_iterator(s.begin(), s.end(), forBoundRe);
                         it != std::sregex_iterator();
                         ++it) {
                        try {
                            size_t v = std::stoull((*it)[1].str());
                            if (v > largestBound) largestBound = v;
                        } catch (...) {
                        }
                    }
                    if (largestBound > 8) {
                        size_t hint = std::min<size_t>(2048, largestBound + 1);
                        for (const auto& p : thisScriptPaths) {
                            auto& cur = context.asset_pool_size_hints[p];
                            if (hint > cur) cur = hint;
                        }
                    }
                }
                return;
            }
            if (j.is_object()) {
                for (auto& [k, v] : j.items()) scan(v);
            } else if (j.is_array()) {
                for (auto& v : j) scan(v);
            }
        };
        scan(json);
        if (! context.script_referenced_layers.empty()) {
            LOG_INFO("Scripts reference %zu named layers", context.script_referenced_layers.size());
        }
        if (! context.registered_asset_paths.empty()) {
            LOG_INFO("Scripts register %zu dynamic assets", context.registered_asset_paths.size());
            for (const auto& p : context.registered_asset_paths) {
                LOG_INFO("  asset: %s", p.c_str());
            }
        }
    }

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
        // For autosize image objects, WPImageObject::FromJson leaves size at
        // the default (2,2) — the real dimensions are only resolved later in
        // ParseImageObj when the texture parser is in scope.  Probe the .tex
        // header here too so the auto-ortho measurement reflects the actual
        // sprite/map dimensions instead of the default placeholder.  Without
        // this the ortho collapses to 2x2 and every world-space-positioned
        // image renders far outside clip space (Aesthetic City 843532366
        // background was ~240,150 in a 2x2 ortho — entirely off-screen).
        WPTexImageParser tempParser(&vfs);
        i32              w = 0, h = 0;
        for (auto& obj : wp_objs) {
            auto* img = std::get_if<wpscene::WPImageObject>(&obj);
            if (img == nullptr) continue;
            i32 iw = (i32)img->size.at(0);
            i32 ih = (i32)img->size.at(1);
            if (img->autosize && iw <= 2 && ih <= 2 &&
                ! img->material.textures.empty() &&
                ! img->material.textures.front().empty()) {
                auto header = tempParser.ParseHeader(img->material.textures.front());
                if (header.isSprite && header.spriteAnim.numFrames() > 0) {
                    const auto& frame = header.spriteAnim.GetCurFrame();
                    iw                = (i32)frame.width;
                    ih                = (i32)frame.height;
                } else if (header.mapWidth > 0 && header.mapHeight > 0) {
                    iw = header.mapWidth;
                    ih = header.mapHeight;
                }
            }
            if (iw * ih > w * h) {
                w = iw;
                h = ih;
            }
        }
        sc.general.orthogonalprojection.width  = w;
        sc.general.orthogonalprojection.height = h;
    }

    InitContext(context, vfs, sc);
    ParseCamera(context, sc);

    // Build group node hierarchy: add each group to its parent (or scene root).
    //
    // Groups whose parent_id refers to an image/text/particle object are
    // deferred — those nodes aren't in node_map until after wp_objs parsing
    // (see ParseImageObj/ParseTextObj).  Without this, e.g. solar system's
    // info-panel chain `725 → 530 → 1210 → 936 → (image 1166) → (group 1234)
    // → (image 970) → (group 974) → (text 982)` collapses: groups 1234 and
    // 974 orphan to scene root, losing the 0.0001× and 10× parent scales.
    // Text 982 then renders at world scale 0.013 instead of 1.8e-5 — the
    // "giant 026-0 text filling the viewport" bug.
    std::vector<GroupInfo> deferred_group_links;
    for (auto& gi : group_infos) {
        auto& node = context.node_map.at(gi.id);
        if (gi.parent_id >= 0 && context.node_map.count(gi.parent_id)) {
            context.node_map.at(gi.parent_id)->AppendChild(node);
        } else if (gi.parent_id >= 0) {
            // Parent is a non-group object parsed later.  Temporarily attach
            // to scene root; re-link after wp_objs parsing below.
            context.scene->sceneGraph->AppendChild(node);
            deferred_group_links.push_back(gi);
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

    // Pre-allocate dynamic-asset pool nodes.  SceneScript calls
    // engine.registerAsset(path) to declare a dynamic image, then creates
    // instances at runtime via thisScene.createLayer(asset).  Since our
    // render graph is static, we pre-build a pool of hidden scene nodes per
    // asset; createLayer/destroyLayer toggle visibility rather than
    // instantiating at runtime.  Particles are skipped — they'd need pool
    // support on the particle pipeline too.
    std::map<i32, std::string> pool_id_to_name;
    {
        // Default 8 slots — plenty for typical coin/spawn patterns.  Scripts
        // that run their own object pool (3body, trail systems) get the
        // larger hint from asset_pool_size_hints.
        const int kDefaultPoolSize = 8;
        i32       synthId          = 2'000'000;
        // Assign pool nodes a json_order LATER than every real scene object.
        // The z-order sort at the end of parse uses json_order; nodes not in
        // the map are treated as cameras and sorted to the FRONT, which means
        // pool coins render before backgrounds and get painted over.  We
        // want them rendered LAST (on top), so base their order on obj_idx +
        // large offset.
        size_t poolOrderBase = obj_idx + 1'000'000;
        for (const auto& assetPath : context.registered_asset_paths) {
            bool isParticle = assetPath.find("particles/") == 0;
            // Sanitize asset path into a unique layer-name prefix.
            std::string safePrefix = "__pool_" + assetPath;
            for (char& c : safePrefix)
                if (c == '/' || c == '.') c = '_';
            // Resolve the on-disk path.  Pools are keyed by the bare
            // assetPath (what the script writes in createLayer/registerAsset),
            // but the actual file may live under
            // `<root>/workshop/<workshopId>/<rest>` for workshop-imported
            // scripts.  Probe both forms so the synthetic pool wpobj's
            // `image` field references a file that actually exists in the
            // VFS — otherwise the wpobj parse fails and the pool stays empty.
            std::string resolvedPath = assetPath;
            if (! isParticle && ! vfs.Contains("/assets/" + assetPath)) {
                auto wid = context.script_asset_workshop_id.find(assetPath);
                if (wid != context.script_asset_workshop_id.end()) {
                    auto slash = assetPath.find('/');
                    if (slash != std::string::npos) {
                        std::string candidate = assetPath.substr(0, slash) +
                                                "/workshop/" + wid->second +
                                                assetPath.substr(slash);
                        if (vfs.Contains("/assets/" + candidate)) {
                            LOG_INFO("  asset workshop-resolved: '%s' → '%s'",
                                     assetPath.c_str(),
                                     candidate.c_str());
                            resolvedPath = candidate;
                        }
                    }
                }
            }
            auto hint      = context.asset_pool_size_hints.find(assetPath);
            int  kPoolSize = hint != context.asset_pool_size_hints.end()
                                 ? static_cast<int>(hint->second)
                                 : kDefaultPoolSize;
            for (int i = 0; i < kPoolSize; i++) {
                std::string poolName = safePrefix + "_" + std::to_string(i);
                i32         thisId   = synthId++;
                // Particle pool layers store the particle path under
                // "particle"; image pool layers store under "image" and rely
                // on the model's autosize to resolve dimensions.  Scripts
                // always set origin at runtime so the off-center zero
                // default isn't an issue.
                nlohmann::json poolObj = {
                    { "id", thisId },     { "name", poolName },  { "origin", "0 0 0" },
                    { "scale", "1 1 1" }, { "angles", "0 0 0" }, { "visible", false },
                };
                if (isParticle)
                    poolObj["particle"] = resolvedPath;
                else
                    poolObj["image"] = resolvedPath;
                size_t beforeSize = wp_objs.size();
                if (isParticle)
                    AddWPObject<wpscene::WPParticleObject>(wp_objs, poolObj, vfs);
                else
                    AddWPObject<wpscene::WPImageObject>(wp_objs, poolObj, vfs);
                if (wp_objs.size() > beforeSize) {
                    context.script_referenced_layers.insert(poolName);
                    context.scene->assetPools[assetPath].push_back(poolName);
                    pool_id_to_name[thisId] = poolName;
                    json_order[thisId]      = poolOrderBase++;
                }
            }
        }
        if (! context.scene->assetPools.empty()) {
            LOG_INFO("Dynamic asset pools allocated: %zu assets", context.scene->assetPools.size());
            for (const auto& [path, names] : context.scene->assetPools) {
                LOG_INFO("  %s × %zu slots", path.c_str(), names.size());
            }
        }
    }

    // Build name→initial state from parsed objects (before node transforms may be reset)
    struct ObjInitState {
        std::array<float, 3> origin;
        std::array<float, 3> scale;
        std::array<float, 3> angles;
        std::array<float, 2> size;
        std::array<float, 2> parallaxDepth;
        bool                 visible;
    };
    std::unordered_map<std::string, ObjInitState> nameToObjState;
    for (WPObjectVar& obj : wp_objs) {
        std::visit(
            visitor::overload {
                [&context, &nameToObjState](wpscene::WPImageObject& obj) {
                    if (! obj.name.empty()) {
                        // Parallax applies visually to layers with
                        // their authored `parallaxDepth` — confirmed
                        // empirically on wallpaper 2866203962 where
                        // the play button is rendered at the
                        // parallax-shifted position even though it
                        // has an effect chain.  Forward the author's
                        // depth straight through so the cursor
                        // hit-test shifts with the rendered visual.
                        nameToObjState[obj.name] = { obj.origin, obj.scale,         obj.angles,
                                                     obj.size,   obj.parallaxDepth, obj.visible };
                    }
                    ParseImageObj(context, obj);
                },
                [&context, &nameToObjState](wpscene::WPParticleObject& obj) {
                    // Particle layers don't have a size in the same
                    // sense as image quads, but nameToObjState is
                    // what the pool-layer registration loop consults
                    // below — populate it so __pool_particles_* nodes
                    // get registered in nodeNameToId / initialStates.
                    if (! obj.name.empty()) {
                        nameToObjState[obj.name] = {
                            obj.origin,        obj.scale,
                            obj.angles,        std::array<float, 2> { 0.0f, 0.0f },
                            obj.parallaxDepth, obj.visible
                        };
                    }
                    ParseParticleObj(context, obj);
                },
                [&context, &sm](wpscene::WPSoundObject& obj) {
                    auto* streamPtr = WPSoundParser::Parse(obj, *context.vfs, sm);
                    if (streamPtr) {
                        if (obj.hasVolumeScript || obj.hasVolumeAnimation) {
                            Scene::SoundVolumeScript svs;
                            svs.script           = obj.volumeScript;
                            svs.scriptProperties = obj.volumeScriptProperties;
                            svs.layerName        = obj.name;
                            svs.initialVolume    = obj.volume;
                            svs.streamPtr        = streamPtr;
                            if (obj.hasVolumeAnimation) {
                                svs.hasAnimation     = true;
                                svs.animation.name   = obj.volumeAnimation.name;
                                svs.animation.mode   = obj.volumeAnimation.mode;
                                svs.animation.fps    = obj.volumeAnimation.fps;
                                svs.animation.length = obj.volumeAnimation.length;
                                for (const auto& kf : obj.volumeAnimation.keyframes) {
                                    svs.animation.keyframes.push_back({ kf.frame, kf.value });
                                }
                                LOG_INFO("sound '%s': volume animation '%s' (%zu keyframes, %.0f "
                                         "frames @ %.0f fps)",
                                         obj.name.c_str(),
                                         svs.animation.name.c_str(),
                                         svs.animation.keyframes.size(),
                                         svs.animation.length,
                                         svs.animation.fps);
                            }
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
                [&context, &nameToObjState](wpscene::WPTextObject& obj) {
                    // Record text-layer transforms so layerInitialStates
                    // gets a non-zero size — required for cursor
                    // hit-testing on draggable text (VHS Time/Date).
                    if (! obj.name.empty()) {
                        nameToObjState[obj.name] = { obj.origin,
                                                     obj.scale,
                                                     obj.angles,
                                                     obj.size,
                                                     std::array<float, 2> { 0.0f, 0.0f },
                                                     obj.visible };
                    }
                    ParseTextObj(context, obj);
                },
            },
            obj);
    }

    // Fix up deferred group links now that image/text/etc. parents are in
    // node_map.  Detach from scene root, re-attach under the real parent, and
    // mark dirty so the next UpdateTrans propagates the full transform chain
    // to children (text/image nodes already attached to these groups).
    for (auto& gi : deferred_group_links) {
        auto pit = context.node_map.find(gi.parent_id);
        if (pit == context.node_map.end()) continue;
        auto& group_node  = context.node_map.at(gi.id);
        auto& parent_node = pit->second;

        // If the new parent is an image with an effect chain, its spImgNode's
        // local transform was reset to identity (line 1515, so the base pass
        // renders at origin into the per-image effect ortho camera).  The
        // authored transform now lives on imgEffectLayer.FinalNode.  Children
        // parented to spImgNode therefore lose the parent's authored scale/
        // origin.  Restore it by pre-composing the parent's authored local
        // into the re-linked group's own local — works for pure
        // scale+translate chains (solar info-panel case; no rotation).
        auto eit = context.scene->nodeEffectLayerMap.find(gi.parent_id);
        if (eit != context.scene->nodeEffectLayerMap.end() && eit->second) {
            auto&           finalNode = eit->second->FinalNode();
            Eigen::Vector3f parent_t  = finalNode.Translate();
            Eigen::Vector3f parent_s  = finalNode.Scale();
            Eigen::Vector3f parent_r  = finalNode.Rotation();
            Eigen::Vector3f group_t   = group_node->Translate();
            Eigen::Vector3f group_s   = group_node->Scale();
            Eigen::Vector3f group_r   = group_node->Rotation();
            if (parent_r.squaredNorm() > 1e-6f || group_r.squaredNorm() > 1e-6f) {
                LOG_ERROR("relink inject: rotation in chain for id=%d unsupported, "
                          "parent_r=(%.3f,%.3f,%.3f) group_r=(%.3f,%.3f,%.3f)",
                          gi.id,
                          parent_r.x(),
                          parent_r.y(),
                          parent_r.z(),
                          group_r.x(),
                          group_r.y(),
                          group_r.z());
            }
            group_node->SetTranslate(Eigen::Vector3f(parent_t.x() + parent_s.x() * group_t.x(),
                                                     parent_t.y() + parent_s.y() * group_t.y(),
                                                     parent_t.z() + parent_s.z() * group_t.z()));
            group_node->SetScale(Eigen::Vector3f(parent_s.x() * group_s.x(),
                                                 parent_s.y() * group_s.y(),
                                                 parent_s.z() * group_s.z()));
            LOG_INFO("relink inject: id=%d parent=%d effect-reset; composed "
                     "local=(scale %.4g×%.4g×%.4g, origin %.2f,%.2f,%.2f)",
                     gi.id,
                     gi.parent_id,
                     group_node->Scale().x(),
                     group_node->Scale().y(),
                     group_node->Scale().z(),
                     group_node->Translate().x(),
                     group_node->Translate().y(),
                     group_node->Translate().z());
        }

        context.scene->sceneGraph->RemoveChild(group_node.get());
        // Propagate MarkTransDirty to all descendants BEFORE the SetParent
        // call marks self dirty — MarkTransDirty short-circuits if already
        // dirty, so order matters for descendants' cached world matrices.
        group_node->SetTranslate(group_node->Translate());
        parent_node->AppendChild(group_node);
        group_node->SetParent(parent_node.get());
        LOG_INFO("relinked deferred group id=%d → parent %d (was orphaned at scene root)",
                 gi.id,
                 gi.parent_id);
    }

    // Recompute original_world_transforms for groups whose links were deferred.
    // The initial pass at group-link time used stale parent OWT (the parent
    // image wasn't parsed yet), so any image child of a deferred group saw
    // an incomplete parent chain.  Walk the group_infos list in insertion
    // order; OWT for a parent group always precedes its children.
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
                    lis.origin        = objIt->second.origin;
                    lis.scale         = objIt->second.scale;
                    lis.angles        = objIt->second.angles;
                    lis.size          = objIt->second.size;
                    lis.parallaxDepth = objIt->second.parallaxDepth;
                    lis.visible       = objIt->second.visible;
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
                // conditionValue is the scene.json comparison target (used
                // for combo bindings that do string equality against the
                // user prop).  Keep this as the authored literal.
                std::string conditionValue;
                if (obj.at("visible").contains("value")) {
                    const auto& visValue = obj.at("visible").at("value");
                    if (visValue.is_boolean()) {
                        conditionValue = visValue.get<bool>() ? "1" : "0";
                    } else if (visValue.is_number_integer()) {
                        conditionValue = std::to_string(visValue.get<int>());
                    } else if (visValue.is_string()) {
                        conditionValue = visValue.get<std::string>();
                    }
                }
                // Alternative format: "user": {"name": "propname", "value": 6}
                if (conditionValue.empty() && userField.is_object() &&
                    userField.contains("value")) {
                    const auto& cv = userField.at("value");
                    if (cv.is_number_integer()) {
                        conditionValue = std::to_string(cv.get<int>());
                    } else if (cv.is_string()) {
                        conditionValue = cv.get<std::string>();
                    }
                }

                // defaultVis must reflect the CURRENT user-property value
                // (override if any, else project.json default), not the
                // scene.json literal.  Otherwise persisted/--set overrides
                // of visibility-bound props don't take effect until the
                // next runtime applyUserPropsRuntime() — which never fires
                // for the INITIAL value at load.  Same resolver the runtime
                // path at SceneWallpaper::applyUserPropsRuntime uses.
                bool defaultVis = true;
                if (g_currentUserProperties) {
                    nlohmann::json resolved =
                        g_currentUserProperties->ResolveValue(obj.at("visible"));
                    if (resolved.is_boolean()) {
                        defaultVis = resolved.get<bool>();
                    } else if (resolved.is_number()) {
                        defaultVis = resolved.get<double>() != 0.0;
                    } else if (resolved.is_string()) {
                        auto s     = resolved.get<std::string>();
                        defaultVis = ! s.empty() && s != "0" && s != "false";
                    }
                } else if (obj.at("visible").contains("value")) {
                    const auto& visValue = obj.at("visible").at("value");
                    if (visValue.is_boolean())
                        defaultVis = visValue.get<bool>();
                    else if (visValue.is_number_integer())
                        defaultVis = visValue.get<int>() != 0;
                    else if (visValue.is_string()) {
                        auto s     = visValue.get<std::string>();
                        defaultVis = ! s.empty() && s != "0";
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

    // Register dynamic-asset pool nodes in nodeNameToId / layerInitialStates.
    // The main loop above iterates the raw scene.json objects only; pool
    // nodes were synthesized in C++ and bypass that registration path.
    // Without this, thisScene.getLayer('__pool_...') returns null and
    // createLayer falls through to its stub.
    for (const auto& [poolId, poolName] : pool_id_to_name) {
        auto nodeIt = context.node_map.find(poolId);
        if (nodeIt == context.node_map.end()) continue;
        auto stateIt = nameToObjState.find(poolName);
        if (stateIt == nameToObjState.end()) continue;
        context.scene->nodeById[poolId]       = nodeIt->second.get();
        context.scene->nodeNameToId[poolName] = poolId;
        Scene::LayerInitialState lis;
        lis.origin                                  = stateIt->second.origin;
        lis.scale                                   = stateIt->second.scale;
        lis.angles                                  = stateIt->second.angles;
        lis.size                                    = stateIt->second.size;
        lis.parallaxDepth                           = stateIt->second.parallaxDepth;
        lis.visible                                 = stateIt->second.visible;
        context.scene->layerInitialStates[poolName] = lis;
    }

    // Sync layerInitialStates visibility with user property bindings.
    // User prop bindings may set nodes invisible AFTER layerInitialStates was populated,
    // causing the JS proxy to think the node is visible when it's actually hidden.
    // Also apply the binding's default visibility directly on the node itself —
    // otherwise m_visible stays at the ctor default (true) until the first
    // applyUserPropertyChanges() (which only fires on user-driven changes).
    // This matters for scene-graph visibility inheritance: a hidden parent
    // group (e.g. 24088 "人物" bound to timevarying==0 while default==1)
    // must actually have m_visible=false at frame 0 so children stop rendering.
    for (const auto& [propName, bindings] : context.scene->userPropVisBindings) {
        for (const auto& binding : bindings) {
            auto* node = binding.node;
            if (! node) continue;
            node->SetVisible(binding.defaultVisible);
            for (auto& [layerName, lis] : context.scene->layerInitialStates) {
                auto nameIt = context.scene->nodeNameToId.find(layerName);
                if (nameIt != context.scene->nodeNameToId.end() && nameIt->second == node->ID()) {
                    lis.visible = binding.defaultVisible;
                }
            }
        }
    }

    // Extract property scripts from raw JSON.  Two attachment shapes:
    //   - objects[N].<prop>                     → attachment=Object
    //   - objects[N].animationlayers[M].<prop>  → attachment=AnimationLayer, idx=M
    // The actual extraction lives in WPPropertyScriptExtract.hpp so tests
    // can exercise both shapes without the full parser.
    for (auto& obj : json.at("objects")) {
        if (! obj.contains("id") || ! obj.at("id").is_number_integer()) continue;
        i32         id = obj.at("id").get<i32>();
        std::string layerName;
        if (obj.contains("name") && obj.at("name").is_string())
            layerName = obj.at("name").get<std::string>();

        wek::extractPropertyScriptsFromHost(id,
                                            layerName,
                                            obj,
                                            ScenePropertyScript::Attachment::Object,
                                            -1,
                                            context.scene->propertyScripts);

        // Particle instance-override scripts (e.g. NieR:Automata's audio-
        // reactive emission-rate modulation on the starfield particles).
        wek::extractParticleInstanceOverrideScripts(id,
                                                    layerName,
                                                    obj,
                                                    ScenePropertyScript::Attachment::Object,
                                                    -1,
                                                    context.scene->propertyScripts);

        // Per-animation-layer scripts (puppet rigs).  Used by Lucy
        // (3521337568) to offset start frames per rigged animation track.
        wek::extractAnimationLayerScripts(id, layerName, obj, context.scene->propertyScripts);
    }

    // Wallpaper 2866203962 (Cyberpunk Lucy music player) fader pattern:
    // its alpha script preserves "exception"-named layers while a song
    // plays, so the track title hovers on screen long after the player
    // fades.  Strip the exception gate so all player layers fade together.
    for (auto& sps : context.scene->propertyScripts) {
        const std::string needle = "playerExceptions.indexOf(element)==-1 || !shared.songplays";
        auto              pos    = sps.script.find(needle);
        if (pos != std::string::npos) {
            sps.script.replace(pos, needle.size(), "true");
            LOG_INFO("Patched player-fader alpha script: dropped exception gate "
                     "(layer id=%d name='%s')",
                     sps.id,
                     sps.layerName.c_str());
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

    // Record scene's HDR intent so SceneWallpaper can decide whether to upgrade
    // render targets.  If the scene declares hdr:false, we should stay in SDR to
    // avoid over-brightness from overbright materials + additive blending.
    //
    // Exception: scenes that declare hdr:false but contain instance-overrides
    // with brightness > 1 (e.g. NieR:Automata 3633635618's "Молния" thunderbolts
    // with brightness=5.0) push particle color channels past 1.0 at spawn; in
    // SDR the fragment clamps per-channel, then additive blending piles the
    // clamped output into pure white — the blue tinge the artist intended is
    // gone.  Force HDR for these scenes so the RGBA16F RTs preserve the channel
    // ratios and FinPass's exposure tonemap compresses back to [0,1] without
    // losing hue.
    bool force_hdr_for_brightness = false;
    const char* env_no_hdr = std::getenv("WEKDE_DISABLE_OVERBRIGHT_HDR");
    const bool  skip_auto_hdr = env_no_hdr && env_no_hdr[0] != '\0' && env_no_hdr[0] != '0';
    if (! skip_auto_hdr && ! sc.general.hdr && json.contains("objects") && json.at("objects").is_array()) {
        for (const auto& obj : json.at("objects")) {
            if (! obj.is_object()) continue;
            if (! obj.contains("instanceoverride")) continue;
            const auto& ov = obj.at("instanceoverride");
            if (! ov.is_object() || ! ov.contains("brightness")) continue;
            const auto& b = ov.at("brightness");
            if (b.is_number() && b.get<float>() > 1.0f) {
                force_hdr_for_brightness = true;
                break;
            }
        }
    }
    if (force_hdr_for_brightness) {
        LOG_INFO("HDR auto-enabled: scene declared hdr:false but has instance-override "
                 "brightness>1; forcing RGBA16F+tonemap to preserve overbright hue");
    }
    const bool effective_hdr   = sc.general.hdr || force_hdr_for_brightness;
    context.scene->hdrContent  = effective_hdr;

    // Pick the bloom parameter variant matching scene's effective HDR mode.  WE
    // scenes carry both SDR (bloomstrength/bloomthreshold) and HDR
    // (bloomhdrstrength/bloomhdrthreshold) values; the SDR threshold is tuned
    // for 0..1 framebuffers while the HDR threshold expects 0..∞ accumulation.
    const float scene_bloom_strength =
        effective_hdr ? sc.general.bloomhdrstrength : sc.general.bloomstrength;
    const float scene_bloom_threshold =
        effective_hdr ? sc.general.bloomhdrthreshold : sc.general.bloomthreshold;

    // Create bloom post-processing passes if enabled
    if (sc.general.bloom) {
        // WE chooses between the legacy LDR bloom path and the modern HDR
        // mip-chain pipeline based on the scene's `postprocessing` enum AND
        // the scene-level `hdr` boolean.  Per asset+binary inspection:
        //   - "ultra"      → scene-HDR  (mip-chain + combine_hdr_upsample)
        //   - "displayhdr" → display-HDR (mip-chain + combine_dhdr_upsample;
        //                    we currently treat this the same as "ultra"
        //                    because we don't expose a display-HDR swapchain
        //                    upgrade — running combine_hdr's SDR LINEAR=0
        //                    branch is the closest equivalent and gives a
        //                    sensible degradation on UNORM swapchains)
        //   - "low" / "medium" / absent → LDR (legacy 4-pass + raw `combine`)
        // The HDR branches additionally require `hdr: true` at scene level;
        // without it the LDR path is forced regardless of postprocessing.
        const auto& pp = sc.general.orthogonalprojection.postprocessing;
        const bool use_hdr_pipeline =
            sc.general.hdr && (pp == "ultra" || pp == "displayhdr");

        LOG_INFO("Bloom enabled: strength=%.2f threshold=%.2f (scene_hdr=%d effective_hdr=%d "
                 "postprocessing='%s' pipeline=%s)",
                 scene_bloom_strength,
                 scene_bloom_threshold,
                 (int)sc.general.hdr,
                 (int)effective_hdr,
                 pp.c_str(),
                 use_hdr_pipeline ? "HDR-mip-chain" : "LDR-legacy");

        auto& scene  = *context.scene;
        auto& vfs    = *context.vfs;
        const auto fullW = static_cast<float>(context.ortho_w);
        const auto fullH = static_cast<float>(context.ortho_h);

      if (use_hdr_pipeline) {
        // 5-level mip chain.  Mip 0 (= scene RT copy) at full res; each next
        // level is halved.  hdr_downsample.frag handles all DS/US/extract
        // variants via combos (BLOOM=1 bright-pass, UPSAMPLE=1 scatter-add,
        // BICUBIC=1 cubic interp).  See WE assets/shaders/hdr_downsample.frag.
        scene.renderTargets[std::string(WE_BLOOM_SCENE)] = {
            .width  = context.ortho_w,
            .height = context.ortho_h,
            .bind   = { .enable = true, .screen = true },
        };
        const std::array<std::pair<std::string_view, float>, 4> mip_specs { {
            { WE_BLOOM_MIP1, 0.5f },
            { WE_BLOOM_MIP2, 0.25f },
            { WE_BLOOM_MIP3, 0.125f },
            { WE_BLOOM_MIP4, 0.0625f },
        } };
        for (auto& [name, scale] : mip_specs) {
            scene.renderTargets[std::string(name)] = {
                .width  = 2,
                .height = 2,
                .bind   = { .enable = true, .screen = true, .scale = scale },
            };
        }

        // Per-pass spec.  `src_scale` is the SOURCE mip's scale (1/(2^src_lvl)),
        // used to compute g_RenderVar0 corner offsets in UV space:
        //   dx = 0.5 / src_w = 0.5 / (fullW * src_scale)
        // The 4-tap shader samples (-dx,-dy), (+dx,-dy), (-dx,+dy), (+dx,+dy)
        // around v_TexCoord, averaging into the destination mip.
        enum class Stage
        {
            Extract, // BLOOM=1, mip0 → mip1
            Down,    // plain 4-tap, mipN → mip(N+1)
            Up,      // UPSAMPLE=1 + additive, mipN → mip(N-1)
            Compose, // combine_hdr final
        };
        struct BloomPassDef {
            Stage                    stage;
            std::vector<std::string> textures; // input texture refs
            std::string              output;   // destination RT
            float                    src_scale; // source mip scale (for offset calc)
        };
        const std::array<BloomPassDef, 8> bloomPasses { {
            // Bright-pass extract: source is mip0 at full res
            { Stage::Extract,
              { std::string(WE_BLOOM_SCENE) },
              std::string(WE_BLOOM_MIP1),
              1.0f },
            // Cascade downsample
            { Stage::Down,
              { std::string(WE_BLOOM_MIP1) },
              std::string(WE_BLOOM_MIP2),
              0.5f },
            { Stage::Down,
              { std::string(WE_BLOOM_MIP2) },
              std::string(WE_BLOOM_MIP3),
              0.25f },
            { Stage::Down,
              { std::string(WE_BLOOM_MIP3) },
              std::string(WE_BLOOM_MIP4),
              0.125f },
            // Cascade upsample (additive — accumulates into the next-larger mip
            // which already holds its downsampled-from-above content).
            { Stage::Up,
              { std::string(WE_BLOOM_MIP4) },
              std::string(WE_BLOOM_MIP3),
              0.0625f },
            { Stage::Up,
              { std::string(WE_BLOOM_MIP3) },
              std::string(WE_BLOOM_MIP2),
              0.125f },
            { Stage::Up,
              { std::string(WE_BLOOM_MIP2) },
              std::string(WE_BLOOM_MIP1),
              0.25f },
            // Final compose: combine_hdr LINEAR=0 path produces clamped linear
            // (sRGB-decoded scene + 4-tap upsampled bloom · exposure).  FinPass
            // sRGB-encodes for display.
            { Stage::Compose,
              { std::string(WE_BLOOM_SCENE), std::string(WE_BLOOM_MIP1) },
              std::string(SpecTex_Default),
              0.5f },
        } };

        scene.bloomConfig.enabled   = true;
        scene.bloomConfig.strength  = scene_bloom_strength;
        scene.bloomConfig.threshold = scene_bloom_threshold;

        for (auto& def : bloomPasses) {
            wpscene::WPMaterial wpmat;
            switch (def.stage) {
            case Stage::Extract:
            case Stage::Down:
            case Stage::Up:      wpmat.shader = "hdr_downsample"; break;
            case Stage::Compose: wpmat.shader = "combine_hdr"; break;
            }
            wpmat.textures = def.textures;

            // Combos drive the shader-side variant.
            if (def.stage == Stage::Extract) wpmat.combos["BLOOM"] = 1;
            if (def.stage == Stage::Up) wpmat.combos["UPSAMPLE"] = 1;
            // Upsample passes additively accumulate into the destination —
            // the destination mip already holds its downsampled-from-above
            // content from a prior Down pass, and the Up pass adds the
            // upsampled smaller mip.  ParseBlendMode("additive") →
            // BlendMode::Additive (srcAlpha*src + 1*dst), which becomes
            // src + dst since the shader writes alpha=1.
            if (def.stage == Stage::Up) wpmat.blending = "additive";
            // Otherwise: extract / downsample / compose all overwrite (alpha=1
            // shader output through Normal blend = pure write).

            auto         spNode = std::make_shared<SceneNode>();
            WPShaderInfo shaderInfo;
            shaderInfo.baseConstSvs = context.global_base_uniforms;
            SceneMaterial     material;
            WPShaderValueData svData;

            if (! LoadMaterial(vfs, wpmat, &scene, spNode.get(), &material, &svData, &shaderInfo)) {
                LOG_ERROR("bloom: failed to load material (stage=%d)", (int)def.stage);
                scene.bloomConfig.enabled = false;
                break;
            }

            LoadConstvalue(material, wpmat, shaderInfo);

            // Per-pass uniforms (set on customShader.constValues directly —
            // LoadConstvalue's name→glname lookup doesn't map direct
            // g_-prefixed uniforms reliably through the alias fallback).
            if (def.stage == Stage::Compose) {
                // combine_hdr g_RenderVar0: .x = exposure, .y = HDR-display
                // smoothstep boost (only used by DISPLAYHDR=1 branch we don't
                // activate; 0 for SDR).
                material.customShader.constValues["g_RenderVar0"] =
                    std::vector<float> { 1.0f, 0.0f, 0.0f, 0.0f };
                // combine_hdr g_TexelSize: 1 texel of g_Texture1 (the bloom
                // mip we sample).  g_Texture1 = WE_BLOOM_MIP1 at 1/2 scale.
                material.customShader.constValues["g_TexelSize"] = std::vector<float> {
                    1.0f / (fullW * 0.5f),
                    1.0f / (fullH * 0.5f),
                };
            } else {
                // hdr_downsample g_RenderVar0: 4-corner offsets in source RT
                // UV space (-dx, -dy, +dx, +dy) where d = 0.5 / src_resolution.
                const float dx = 0.5f / (fullW * def.src_scale);
                const float dy = 0.5f / (fullH * def.src_scale);
                material.customShader.constValues["g_RenderVar0"] =
                    std::vector<float> { -dx, -dy, dx, dy };

                if (def.stage == Stage::Extract) {
                    // BLOOM=1 branch reads g_BloomBlendParams (knee shape),
                    // g_BloomTint, g_BloomStrength.  Soft knee from
                    // (threshold - knee) ramping to (threshold + knee):
                    //   .x = threshold (hard cutoff)
                    //   .y = threshold - knee  (knee start)
                    //   .z = 2*knee            (knee width)
                    //   .w = 0.25 / knee       (quadratic shaping factor)
                    // knee=0.30 gives a smooth roll-on around scene_threshold.
                    const float knee = 0.30f;
                    material.customShader.constValues["g_BloomBlendParams"] =
                        std::vector<float> {
                            scene_bloom_threshold,
                            scene_bloom_threshold - knee,
                            2.0f * knee,
                            0.25f / knee,
                        };
                    material.customShader.constValues["g_BloomTint"] =
                        std::vector<float> { 1.0f, 1.0f, 1.0f };
                    material.customShader.constValues["g_BloomStrength"] =
                        std::vector<float> { scene_bloom_strength };
                }
                if (def.stage == Stage::Up) {
                    // UPSAMPLE=1 branch reads g_BloomScatter (multiplies the
                    // 4-tap average).  1.0 = neutral pass-through scatter.
                    material.customShader.constValues["g_BloomScatter"] =
                        std::vector<float> { 1.0f };
                }
            }

            auto spMesh = std::make_shared<SceneMesh>();
            spMesh->AddMaterial(std::move(material));
            spMesh->ChangeMeshDataFrom(scene.default_effect_mesh);
            spNode->AddMesh(spMesh);
            spNode->SetCamera("effect");

            context.shader_updater->SetNodeData(spNode.get(), svData);

            scene.bloomConfig.outputs.push_back(def.output);
            scene.bloomConfig.nodes.push_back(spNode);

            LOG_INFO("bloom: stage=%d shader='%s' → '%s' created",
                     (int)def.stage,
                     wpmat.shader.c_str(),
                     def.output.c_str());
        }
      } else {
        // Legacy LDR bloom: 4 passes (bright-pass quarter, eighth blur-V,
        // blur-H, raw `combine` add).  Output is unbounded linear; FinPass
        // hard-clips and sRGB-encodes for display.  Matches WE's `combine_ldr`
        // material chain for non-"ultra" scenes.
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

        struct LegacyBloomPassDef {
            std::string              shader;
            std::vector<std::string> textures;
            std::string              output;
        };
        const std::array<LegacyBloomPassDef, 4> legacyPasses { {
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
        } };

        scene.bloomConfig.enabled   = true;
        scene.bloomConfig.strength  = scene_bloom_strength;
        scene.bloomConfig.threshold = scene_bloom_threshold;

        for (auto& def : legacyPasses) {
            wpscene::WPMaterial wpmat;
            wpmat.shader   = def.shader;
            wpmat.textures = def.textures;

            if (def.shader == "downsample_quarter_bloom") {
                wpmat.constantshadervalues["bloomstrength"]  = { scene_bloom_strength };
                wpmat.constantshadervalues["bloomthreshold"] = { scene_bloom_threshold };
            }

            auto         spNode = std::make_shared<SceneNode>();
            WPShaderInfo shaderInfo;
            shaderInfo.baseConstSvs = context.global_base_uniforms;
            SceneMaterial     material;
            WPShaderValueData svData;

            if (! LoadMaterial(vfs, wpmat, &scene, spNode.get(), &material, &svData, &shaderInfo)) {
                LOG_ERROR("bloom (LDR): failed to load material for '%s'",
                          def.shader.c_str());
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

            LOG_INFO("bloom (LDR): pass '%s' → '%s' created",
                     def.shader.c_str(),
                     def.output.c_str());
        }
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

    // Filter dead effects whose shaders failed to compile (workshop shaders
    // that didn't survive HLSL→GLSL — e.g. workshop/2487531853 lens_flare_sun
    // on the Naruto-family wallpapers).  Without this, ResolveEffect would
    // still target the failed effect as last_output and rewrite its output
    // to _rt_default — but the failed pass is dropped at prepare time, so
    // nothing actually composes the chain into _rt_default and the layer
    // disappears (or only the BG behind it shows through).  Removing the
    // dead effects lets ResolveEffect promote the previous good effect as
    // last_output and the chain composes correctly.
    {
        std::size_t total_removed = 0;
        for (auto& [_, cam] : context.scene->cameras) {
            if (! cam || ! cam->HasImgEffect()) continue;
            auto& effs = cam->GetImgEffect();
            if (! effs) continue;
            std::size_t r = effs->RemoveFailedEffects();
            total_removed += r;
        }
        if (total_removed > 0) {
            LOG_INFO("Effect chain repair: dropped %zu effect(s) with failed shader "
                     "compile so chains compose correctly via earlier-good last_output",
                     total_removed);
        }
    }

    WPShaderParser::FinalGlslang();
    WPTextRenderer::Shutdown();

    // End-of-parse hide-pattern summary — prints once to stderr so it's easy
    // to see what the --hide-pattern flag filtered out without greping logs.
    if (! context.hidden_names.empty()) {
        std::cerr << "HidePattern: filtered " << context.hidden_names.size()
                  << " scene object(s) matching '" << context.hide_pattern << "':\n";
        for (const auto& s : context.hidden_names) {
            std::cerr << "  - " << s << '\n';
        }
        std::cerr.flush();
    }

    return context.scene;
}
