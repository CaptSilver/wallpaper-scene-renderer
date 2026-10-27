#include "SceneToRenderGraph.hpp"

#include <cassert>
#include <cstdint>
#include <unordered_set>

#include "Scene/Scene.h"
#include "RenderGraph/RenderGraph.hpp"
#include "SpecTexs.hpp"
#include "Utils/Logging.h"
#include "Core/MapSet.hpp"

#include "VulkanRender/AllPasses.hpp"
#include "VulkanRender/VolumetricChain.hpp"
#include "VulkanRender/SkyboxPass.hpp"

using namespace wallpaper;
namespace wallpaper::rg
{

void doCopy(RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc, TexNode* in, TexNode* out) {
    builder.read(in);
    builder.write(out);

    desc.src = in->key();
    desc.dst = out->key();
}
void addCopyPass(RenderGraph& rgraph, TexNode* in, TexNode* out) {
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [&in, &out](RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc) {
            doCopy(builder, desc, in, out);
        });
}

void addCopyPass(RenderGraph& rgraph, const TexNode::Desc& in, const TexNode::Desc& out,
                 bool flipY = false) {
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [&in, &out, flipY](RenderGraphBuilder& builder, vulkan::CopyPass::Desc& desc) {
            auto* in_node  = builder.createTexNode(in);
            auto* out_node = builder.createTexNode(out, true);
            desc.flipY     = flipY;
            doCopy(builder, desc, in_node, out_node);
        });
}

TexNode* addCopyPass(RenderGraph& rgraph, TexNode* in, TexNode::Desc* out_desc = nullptr) {
    TexNode* copy { nullptr };
    rgraph.addPass<vulkan::CopyPass>(
        "copy",
        PassNode::Type::Copy,
        [&copy, in, out_desc](RenderGraphBuilder& builder, vulkan::CopyPass::Desc& pdesc) {
            auto desc = out_desc == nullptr ? in->genDesc() : *out_desc;
            if (out_desc == nullptr) {
                desc.key += "_" + std::to_string(in->version()) + "_copy";
                desc.name += "_" + std::to_string(in->version()) + "_copy";
            }
            copy = builder.createTexNode(desc, true);
            doCopy(builder, pdesc, in, copy);
        });
    return copy;
}

static TexNode::Desc createTexDesc(std::string path) {
    return TexNode::Desc { .name = path,
                           .key  = path,
                           .type = IsSpecTex(path) ? TexNode::TexType::Temp
                                                   : TexNode::TexType::Imported };
}
} // namespace wallpaper::rg

static void TraverseNode(const std::function<void(SceneNode*)>& func, SceneNode* node) {
    func(node);
    for (auto& child : node->GetChildren()) TraverseNode(func, child.get());
}

static void CheckAndSetSprite(Scene& scene, vulkan::CustomShaderPass::Desc& desc,
                              std::span<const std::string> texs) {
    for (usize i = 0; i < texs.size(); i++) {
        auto& tex = texs[i];
        if (! tex.empty() && ! IsSpecTex(tex) && scene.textures.count(tex) != 0) {
            const auto& stex = scene.textures.at(tex);
            if (stex.isSprite) {
                desc.sprites_map[i] = stex.spriteAnim;
            }
        }
    }
}

struct DelayLinkInfo {
    rg::NodeID id;
    rg::NodeID link_id;
    i32        tex_index;
    rg::NodeID owner_img_id;
};

// A (node id, effect index) pair packed into one key for
// ExtraInfo::hideable_effect_keys -- cheaper than a pair-keyed set and the
// two fields are each well within 32 bits (scene object ids and effect
// counts never approach it).
inline uint64_t PackNodeEffectKey(i32 nodeId, i32 effectIndex) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(nodeId)) << 32) |
           static_cast<uint32_t>(effectIndex);
}

// What ToGraphPass needs, only for a hideable pass, to wire up its runtime
// passthrough. The whole effect chain and the pass's index in it are read for
// per-effect visibility every frame. A Terminal registers the layer's two
// physical ping-pong buffers (bufferA/bufferB) so either can be chosen at
// runtime; a Middle registers just `chainInput`, the one buffer it copies.
// Built in LoadEffectChain before the pass's own ToGraphPass call -- see
// CustomShaderPass::SelectPassthroughSource.
struct HideablePassInfo {
    vulkan::HideableKind                 kind { vulkan::HideableKind::None };
    std::vector<const SceneImageEffect*> chainEffects;
    usize                                chainIndex { 0 };
    std::string                          bufferA, bufferB; // Terminal: both chain buffers
    std::string                          chainInput;       // Middle: the one buffer it copies
    bool                                 baseImageWritten { true };
};

struct ExtraInfo {
    Map<size_t, rg::TexNode*>  id_link_map {};
    Map<size_t, rg::TexNode*>  id_preeffect_map {};
    std::vector<DelayLinkInfo> link_info {};
    rg::RenderGraph*           rgraph { nullptr };
    Scene*                     scene { nullptr };
    bool                       use_mipmap_framebuffer { false };

    // Every (node id, effect index) pair that carries a script or
    // user-property visibility binding, built once from
    // scene.propertyScripts / scene.userPropEffectVisBindings before the
    // graph traversal starts.  LoadEffectChain consults this per effect to
    // decide which chain-writing passes are hideable.  Layers listed in
    // Scene::scriptEffectVisLayers are a second source: all of their effects
    // are hideable, read there directly.
    std::unordered_set<uint64_t> hideable_effect_keys {};
};

// Register a self-write-hazard copy's destination as a name-bound render
// target scaled relative to its source, instead of leaving it for
// CopyPass::prepare()'s frame-time fallback (full clone of the source,
// including its full-resolution bind).
//
// `output` must already be screen-bound or static (never touched by either
// pass) when this runs. VulkanRender.cpp's setRenderTargetSize() resolves
// sizes in two passes over an unordered_map: pass one sizes screen-bound
// targets from the device extent, pass two sizes name-bound targets by
// looking up their bind target's *already-resolved* size. There's no
// ordering guarantee across two name-bound entries chained together, so
// binding our copy's name to a source that is itself only resolved in pass
// two could read a stale or zero size depending on map iteration order.
static void registerScaledHazardCopyTarget(Scene& scene, std::string_view output,
                                           std::string_view copy_key, double scale) {
    auto* src_rt = scene.tryGetRenderTarget(std::string(output));
    if (src_rt == nullptr) {
        LOG_ERROR("self-write hazard source '%.*s' has no registered render target; "
                  "leaving its copy at CopyPass::prepare's full-size clone fallback",
                  (int)output.size(),
                  output.data());
        return;
    }
    if (! (src_rt->bind.screen || ! src_rt->bind.enable)) {
        LOG_ERROR("self-write hazard source '%.*s' is itself name-bound; leaving its "
                  "copy at CopyPass::prepare's full-size clone fallback",
                  (int)output.size(),
                  output.data());
        assert(false && "hazard-copy source must be screen-bound or static");
        return;
    }
    SceneRenderTarget copy_rt = *src_rt;
    copy_rt.allowReuse        = true;
    copy_rt.bind = { .enable = true, .name = std::string(output), .screen = false, .scale = scale };
    scene.renderTargets[std::string(copy_key)] = copy_rt;
}

// Self-write-hazard copies still happen once per reader, at that reader's own
// z-order instant -- see the mechanism note on the hazard branch below for why
// that count can't shrink safely.  This only shrinks each copy's resolution.
// 0.5 was chosen and confirmed by a windowed render comparison against the
// unmodified renderer (no visible softening of hard-edged REFRACT distortion
// at native or 2-4x zoom); not exposed as a user-facing setting.
static constexpr double kSelfWriteHazardCopyScale = 0.5;

// Options the two texture-read call sites (ToGraphPass, addReflectionPass) disagree on.
// Defaults match ToGraphPass's fuller behavior; addReflectionPass opts out of each --
// see the call site comments for why this dedup doesn't change any of the three.
struct TextureReadOptions {
    bool checkSelfWriteHazard { true };
    bool registerSpecLinkTex { true };
    bool trackMipmapFramebuffer { true };
    // Receives the graph node bound to textures[0]; stays null when that slot is
    // empty or a deferred link. Set only on the pass's own material read.
    rg::TexNode** slot0 { nullptr };
};

// Extra per-pass behaviour ToGraphPass callers opt into.
struct ToGraphPassOptions {
    // The pass is the on-screen terminal of an isolated link source: readers'
    // `_rt_link_<id>` copy is taken from this pass's slot-0 input (the sprite-sized
    // chain output) instead of from the _rt_default version it writes.  Any
    // terminal that reads the chain in slot 0 serves: the synthesized
    // passthrough, a puppet's skinned composite, or a colorBlendMode passthrough.
    bool link_from_slot0 { false };
};

// Shared by ToGraphPass and addReflectionPass: bind every texture in `textures` as a
// graph-node read, in order, appending the resolved (or empty, for a deferred/omitted
// slot) key to pdesc.textures. `passId`/`ownerImgId` are only read when
// opts.registerSpecLinkTex is true; pass 0 for either when the caller sets it false.
static void addTextureReads(rg::RenderGraphBuilder& builder, ExtraInfo& extra,
                            rg::RenderGraph& rgraph, const std::vector<std::string>& textures,
                            vulkan::CustomShaderPass::Desc& pdesc, std::string_view output,
                            rg::NodeID passId, i32 ownerImgId, const TextureReadOptions& opts) {
    for (usize i = 0; i < textures.size(); i++) {
        const auto&  url = textures[i];
        rg::TexNode* input { nullptr };
        if (url.empty()) {
            pdesc.textures.emplace_back("");
            continue;
        } else if (opts.registerSpecLinkTex && IsSpecLinkTex(url)) {
            auto id = ParseLinkTex(url);
            extra.link_info.push_back(DelayLinkInfo { .id           = passId,
                                                      .link_id      = id,
                                                      .tex_index    = (i32)i,
                                                      .owner_img_id = (rg::NodeID)ownerImgId });
            pdesc.textures.emplace_back("");
            continue;
        } else if (IsSpecLinkTex(url)) {
            // opts.registerSpecLinkTex == false: bind an unregistered link tex
            // empty, same as an omitted texture, instead of deferring it into
            // extra.link_info. addReflectionPass is the only caller that opts
            // out today -- see its call site comment below for why.
            pdesc.textures.emplace_back("");
            continue;
        } else {
            rg::TexNode::Desc desc;
            desc.key  = url;
            desc.name = url;
            desc.type =
                ! IsSpecTex(url) ? rg::TexNode::TexType::Imported : rg::TexNode::TexType::Temp;
            input = builder.createTexNode(desc);
            if (IsSpecTex(url)) builder.markVirtualWrite(input);
            if (opts.trackMipmapFramebuffer && sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER))
                extra.use_mipmap_framebuffer = true;
        }

        if (opts.checkSelfWriteHazard && url == output) {
            builder.markSelfWrite(input);
            input = rg::addCopyPass(rgraph, input);
            registerScaledHazardCopyTarget(
                *extra.scene, output, input->key(), kSelfWriteHazardCopyScale);
        }
        builder.read(input);
        if (i == 0 && opts.slot0 != nullptr) *opts.slot0 = input;
        pdesc.textures.emplace_back(input->key());
    }
}

static void ToGraphPass(SceneNode* node, std::string_view output, i32 imgId, ExtraInfo& extra,
                        const HideablePassInfo*   hideable = nullptr,
                        const ToGraphPassOptions& options  = {});

static void LoadEffectChain(SceneNode* node, SceneImageEffectLayer* effs, ExtraInfo& extra) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    effs->ResolveEffect(scene.default_effect_mesh, "effect");

    // Which effects can be hidden at runtime -- a declared binding, or any
    // effect of a layer a script toggles effects on -- checked once per layer
    // rather than per node.  The whole chain is gathered only when some effect
    // is bound -- the overwhelming majority of layers have none.
    SceneNode*                           terminal_node = effs->ResolvedLastOutput();
    std::vector<const SceneImageEffect*> chain;
    std::vector<bool>                    bound(effs->EffectCount(), false);
    bool                                 anyBound = false;
    const bool layerScripted = scene.scriptEffectVisLayers.count(node->ID()) != 0;
    for (usize j = 0; j < effs->EffectCount(); j++) {
        // The synthesized passthrough is always drawn and is the layer's on-screen
        // terminal, so a hidden effect before it always has a visible successor.
        if (effs->GetEffect(j)->synthesized) continue;
        bound[j] = layerScripted ||
                   extra.hideable_effect_keys.count(PackNodeEffectKey(node->ID(), (i32)j)) != 0;
        anyBound = anyBound || bound[j];
    }
    if (anyBound) {
        chain.reserve(effs->EffectCount());
        for (usize j = 0; j < effs->EffectCount(); j++) chain.push_back(effs->GetEffect(j).get());
    }
    // A compose layer without copybackground never draws its own image into
    // buffer A, so a hidden first effect has nothing valid to copy.
    const bool baseImageWritten = ! (effs->IsPassthrough() && ! effs->CopyBackground());

    for (usize i = 0; i < effs->EffectCount(); i++) {
        auto& eff     = effs->GetEffect(i);
        auto  cmdItor = eff->commands.begin();
        auto  cmdEnd  = eff->commands.end();
        int   nodePos = 0;
        if (bound[i] && eff->chain_writer == nullptr) {
            LOG_INFO("id=%d effect %zu writes only private buffers; it cannot copy its input "
                     "when hidden",
                     node->ID(),
                     i);
        }
        for (auto& n : eff->nodes) {
            if (cmdItor != cmdEnd && nodePos == cmdItor->afterpos) {
                rg::addCopyPass(
                    rgraph, rg::createTexDesc(cmdItor->src), rg::createTexDesc(cmdItor->dst));
                cmdItor++;
            }
            auto& name = n.output;
            if (n.sceneNode->HasMaterial()) {
                auto&       texs = n.sceneNode->Mesh()->Material()->textures;
                std::string tex_list;
                for (usize t = 0; t < texs.size(); t++) {
                    if (t > 0) tex_list += ", ";
                    tex_list += texs[t].empty() ? "(empty)" : texs[t];
                }
                LOG_INFO("  id=%d eff[%zu].%d: out='%.*s' cam='%s' blend=%d texs=[%s]",
                         node->ID(),
                         i,
                         nodePos,
                         (int)name.size(),
                         name.data(),
                         n.sceneNode->Camera().c_str(),
                         (int)n.sceneNode->Mesh()->Material()->blenmode,
                         tex_list.c_str());
            }
            HideablePassInfo        info;
            const HideablePassInfo* infoPtr = nullptr;
            if (anyBound && bound[i]) {
                if (n.sceneNode.get() == terminal_node) {
                    info    = { vulkan::HideableKind::Terminal,
                                chain,
                                i,
                                std::string(effs->FirstTarget()),
                                std::string(effs->SecondTarget()),
                                {},
                                baseImageWritten };
                    infoPtr = &info;
                } else if (n.sceneNode.get() == eff->chain_writer) {
                    info    = { vulkan::HideableKind::Middle,
                                chain,
                                i,
                                {},
                                {},
                                eff->chain_input,
                                baseImageWritten };
                    infoPtr = &info;
                }
            }
            ToGraphPassOptions options;
            options.link_from_slot0 = effs->IsIsolatedLink() && n.sceneNode.get() == terminal_node;
            ToGraphPass(n.sceneNode.get(), name, node->ID(), extra, infoPtr, options);
            nodePos++;
        }
    }
}

static void ToGraphPass(SceneNode* node, std::string_view output, i32 imgId, ExtraInfo& extra,
                        const HideablePassInfo* hideable, const ToGraphPassOptions& options) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    auto loadEffect = [node, &extra](SceneImageEffectLayer* effs) {
        LoadEffectChain(node, effs, extra);
    };

    if (node->Mesh() == nullptr) return;
    auto* mesh = node->Mesh();
    if (mesh->Material() == nullptr) return;
    auto* material = mesh->Material();

    SceneImageEffectLayer* imgeff = nullptr;
    if (! node->Camera().empty()) {
        // A node naming a camera the parser never registered would throw
        // out_of_range from .at() onto the render worker thread; guard+return.
        if (! exists(scene.cameras, node->Camera())) {
            LOG_ERROR("node camera '%s' not registered", node->Camera().c_str());
            return;
        }
        auto& cam = scene.cameras.at(node->Camera());
        if (cam->HasImgEffect()) {
            imgeff = cam->GetImgEffect().get();
            output = imgeff->FirstTarget();
        }
    }

    // Passthrough compose layers capture _rt_default into the layer's first
    // pingpong, then run the effect chain.  The capture must apply the layer's
    // world transform to UV sampling — the composelayer shader does exactly
    // this (samples _rt_FullFrameBuffer at the layer vertex's world-NDC
    // position), producing a pingpong whose center maps to the layer ORIGIN
    // in framebuffer space.  When the final effect draw renders at the layer's
    // screen position, the shifted-sample and shifted-draw cancel: pixel at
    // layer-origin shows framebuffer at layer-origin (identity).
    //
    // A plain Copy here leaves the pingpong un-shifted (center = framebuffer
    // center).  The final draw then composites scene-center content at the
    // layer's origin — visible as a CHARACTER GHOST when the layer is
    // off-center.  Long Train (1457581889) godrays compose layer at origin
    // (764.9, 506.1) — shifted ~196px left of scene center — ghosts the
    // character (drawn before the compose) onto the left of the screen.
    //
    // Falling through to the normal CustomShaderPass path below executes the
    // layer's own composelayer-shader material, which produces the correctly
    // shifted pingpong.  Z-order is preserved (the pass is still added at the
    // compose layer's scene-graph traversal point) so foreground elements
    // drawn after the compose still render on top.
    //
    // Exception: copybackground=false suppresses BOTH the screen capture and
    // the base pass.  Authors set this when they populate the pingpong via
    // child scene-graph passes (e.g. Nightingale 3470764447 id=249 with
    // colorBlendMode=21 BlendReflect, where capturing the screen amplifies
    // the day-character's blue TV-glow into a hard-edged rectangle).
    if (imgeff != nullptr && imgeff->IsPassthrough() && ! imgeff->CopyBackground()) {
        LOG_INFO("passthrough compose: skipping base+copy (copybackground=false) for '%.*s'",
                 (int)output.size(),
                 output.data());
        loadEffect(imgeff);
        return;
    }

    // Invisible nodes without effects render to an offscreen RT so they don't
    // composite into the main scene but remain accessible via id_link_map.
    // Use the effect camera and default mesh so the image fills the entire
    // offscreen RT — same approach as SceneImageEffectLayer's offscreen path.
    // The global camera would clip content since parent-group transforms place
    // these nodes outside its visible area.
    std::string offscreen_output;
    if (imgeff == nullptr && node->IsOffscreen()) {
        offscreen_output = GenOffscreenRT(imgId);
        output           = offscreen_output;
        // Normal overwrites rather than blends: the RT's first writer CLEARs
        // it on entry (CustomShaderPass), and a sprite that landed alpha-weighted
        // would be weighted again wherever it is read.
        material->blenmode = BlendMode::Normal;
        auto default_node  = SceneNode();
        node->SetCamera("effect");
        node->CopyTrans(default_node);
        node->InheritParent(default_node); // clear parent → no group transform
        mesh->ChangeMeshDataFrom(scene.default_effect_mesh);
    }

    std::string passName = material->name;

    rgraph.addPass<vulkan::CustomShaderPass>(
        passName,
        rg::PassNode::Type::CustomShader,
        [material,
         node,
         imgeff,
         &output,
         &imgId,
         &rgraph,
         &scene,
         &extra,
         hideable,
         link_from_slot0 = options.link_from_slot0](rg::RenderGraphBuilder&         builder,
                                                    vulkan::CustomShaderPass::Desc& pdesc) {
            const auto& pass = builder.workPassNode();
            pdesc.node       = node;
            pdesc.output     = output;
            CheckAndSetSprite(scene, pdesc, material->textures);
            // Taken from the material's own read only, never from the hideable
            // extra reads below: those bind the chain's ping-pong buffers, not the
            // slot an isolated link terminal copies from.  Whatever the terminal is
            // (the synthesized passthrough, a puppet composite, a colorBlendMode
            // passthrough), slot 0 is the chain output it reads.
            rg::TexNode* slot0 { nullptr };
            addTextureReads(builder,
                            extra,
                            rgraph,
                            material->textures,
                            pdesc,
                            output,
                            pass.ID(),
                            imgId,
                            TextureReadOptions { .slot0 = &slot0 });

            if (hideable != nullptr) {
                pdesc.hideable                 = hideable->kind;
                pdesc.chain_effects            = hideable->chainEffects;
                pdesc.chain_index              = (i32)hideable->chainIndex;
                pdesc.chain_base_image_written = hideable->baseImageWritten;
                const TextureReadOptions extraRead { .checkSelfWriteHazard   = false,
                                                     .registerSpecLinkTex    = false,
                                                     .trackMipmapFramebuffer = false };
                if (hideable->kind == vulkan::HideableKind::Terminal) {
                    // Register BOTH of the chain's ping-pong buffers as extra
                    // reads, independent of which single slot the terminal's
                    // own (real) material happens to read -- execute() picks
                    // between them every frame (SelectPassthroughSource)
                    // instead of trusting a single slot resolved once here.
                    // Both names are always registered render targets for this
                    // node regardless of chain length (WPSceneParser sets up
                    // effect_ppong_a/b whenever a layer has effects at all),
                    // so neither read can dangle.
                    addTextureReads(builder,
                                    extra,
                                    rgraph,
                                    { hideable->bufferA, hideable->bufferB },
                                    pdesc,
                                    output,
                                    pass.ID(),
                                    imgId,
                                    extraRead);
                    pdesc.passthrough_input_tex_index_a = (i32)(pdesc.textures.size() - 2);
                    pdesc.passthrough_input_tex_index_b = (i32)(pdesc.textures.size() - 1);
                } else {
                    // One extra read: the buffer this effect copies when hidden.
                    // Not both buffers -- the other one is this pass's own output.
                    addTextureReads(builder,
                                    extra,
                                    rgraph,
                                    { hideable->chainInput },
                                    pdesc,
                                    output,
                                    pass.ID(),
                                    imgId,
                                    extraRead);
                    pdesc.passthrough_input_tex_index_a = (i32)(pdesc.textures.size() - 1);
                }
            }

            rg::TexNode* output_node { nullptr };
            output_node =
                builder.createTexNode(rg::TexNode::Desc { .name = output.data(),
                                                          .key  = output.data(),
                                                          .type = rg::TexNode::TexType::Temp },
                                      true);
            builder.write(output_node);
            if (link_from_slot0 && slot0 != nullptr) {
                // Readers snapshot the sprite this on-screen pass draws from. Its
                // _rt_default write is the whole framebuffer, which would hand them
                // everything drawn so far instead of the layer.
                extra.id_link_map[(usize)imgId] = slot0;
            } else if (output == SpecTex_Default || output == GenOffscreenRT(imgId)) {
                if (link_from_slot0) {
                    LOG_ERROR("id=%d isolated link terminal has no slot-0 input; readers get "
                              "the framebuffer",
                              imgId);
                }
                extra.id_link_map[(usize)imgId] = output_node;
            }
            // A layer's own composite read (WPSceneParser's _a/_b routing) can only
            // ever mean "this layer's content before its own effects ran" -- the
            // post-effect result hasn't been produced yet when the effect chain
            // itself is what's reading it.  Record this write (the pre-effect
            // pingpong capture, present whenever the layer has an imgeff) so the
            // self-reference branch in the resolution loop below has somewhere
            // topologically-upstream to bind instead of a copy of its own
            // not-yet-finished output.
            if (imgeff != nullptr) {
                extra.id_preeffect_map[(usize)imgId] = output_node;
            }
        });

    // load effect
    if (imgeff != nullptr) loadEffect(imgeff);
}

// Render a scene node into _rt_Reflection using the reflected camera.
// Skips nodes that read _rt_Reflection (the reflecting surface itself) and
// offscreen nodes.  Runtime visibility is deliberately NOT consulted here:
// the graph is built once at scene compile, while a layer can be hidden and
// shown again at any time.  CustomShaderPass::execute drops the record for a
// hidden node instead.
static void addReflectionPass(SceneNode* node, ExtraInfo& extra) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    if (! node->Mesh()) return;
    auto* material = node->Mesh()->Material();
    if (! material) return;

    // Skip nodes that read _rt_Reflection (e.g., grid floor)
    for (auto& url : material->textures) {
        if (url == WE_REFLECTION) return;
    }

    // Skip offscreen nodes
    if (node->IsOffscreen()) return;

    // Skip effect camera nodes (internal compositing passes)
    if (! node->Camera().empty()) {
        // Guard the unordered_map .at() against an unregistered camera name
        // (would otherwise throw onto the render worker thread).
        if (! exists(scene.cameras, node->Camera())) {
            LOG_ERROR("node camera '%s' not registered (reflection pass)", node->Camera().c_str());
            return;
        }
        auto& cam = scene.cameras.at(node->Camera());
        if (cam->HasImgEffect()) return;
    }

    std::string passName = "refl_" + material->name;

    rgraph.addPass<vulkan::CustomShaderPass>(
        passName,
        rg::PassNode::Type::CustomShader,
        [material, node, &scene, &rgraph, &extra](rg::RenderGraphBuilder&         builder,
                                                  vulkan::CustomShaderPass::Desc& pdesc) {
            pdesc.node               = node;
            pdesc.output             = std::string(WE_REFLECTION);
            pdesc.camera_override    = "reflected_perspective";
            pdesc.disableDepth       = false;
            pdesc.flipCullMode       = false; // col(1) and row(1) negate cancel winding
            pdesc.useReflectionDepth = true;
            CheckAndSetSprite(scene, pdesc, material->textures);

            // All three knobs off, unchanged from this function's behavior before
            // the addTextureReads extraction -- this is a pure dedup, so every
            // option here reproduces what this loop already did, not a decision
            // that any of the three should now behave differently.
            //  - checkSelfWriteHazard: moot regardless -- the early return above
            //    already skips any node whose material reads WE_REFLECTION, so
            //    url == output (pdesc.output is WE_REFLECTION here) can never be
            //    true by the time this loop runs.
            //  - registerSpecLinkTex: this call has no real pass id or owner image
            //    id to record (passId/ownerImgId are both 0 below, not looked up
            //    from the graph the way ToGraphPass's pass.ID()/imgId are), so
            //    deferring a link tex here would attach it to whichever unrelated
            //    pass or image id happens to be 0 instead of failing loudly.
            //  - trackMipmapFramebuffer: extra.use_mipmap_framebuffer is one flag
            //    shared across every addTextureReads call in this graph build, and
            //    this call has never set it. Whether a reflection material can
            //    legitimately need the mip-mapped framebuffer copy is unverified --
            //    no wallpaper on this box ever reaches addReflectionPass at all
            //    (its camera gate needs a 3D scene with a 2+-keyframe camera path).
            addTextureReads(builder,
                            extra,
                            rgraph,
                            material->textures,
                            pdesc,
                            pdesc.output,
                            /*passId=*/0,
                            /*ownerImgId=*/0,
                            TextureReadOptions { .checkSelfWriteHazard   = false,
                                                 .registerSpecLinkTex    = false,
                                                 .trackMipmapFramebuffer = false });

            auto* output_node =
                builder.createTexNode(rg::TexNode::Desc { .name = std::string(WE_REFLECTION),
                                                          .key  = std::string(WE_REFLECTION),
                                                          .type = rg::TexNode::TexType::Temp },
                                      true);
            builder.write(output_node);
        });
}

namespace wallpaper::vulkan
{

namespace
{
// Count renderable nodes (mesh + material) in the scene graph.
size_t countRenderableNodes(const Scene& scene) {
    if (! scene.sceneGraph) return 0;
    size_t renderable = 0;
    TraverseNode(
        [&renderable](SceneNode* node) {
            if (node->HasMaterial()) renderable++;
        },
        const_cast<SceneNode*>(scene.sceneGraph.get()));
    return renderable;
}
} // namespace

bool skyboxHasForegroundNodes(const Scene& scene) {
    if (! scene.has_skybox) return false;
    // More renderable nodes than skybox layers → the extra ones are foreground
    // content that renders flat in front of the panorama.
    return countRenderableNodes(scene) > scene.skyboxLayerIds.size();
}

void emitSkyboxPass(rg::RenderGraph& rgraph, Scene& scene) {
    // Three-part gate — any miss leaves the graph untouched so a non-skybox (or
    // half-detected) scene emits nothing and the flat path stays byte-identical.
    if (! scene.has_skybox) return;
    if (scene.skyboxLayerIds.empty()) return;
    if (scene.skyboxTexKey.empty()) return;

    const std::string pano_key = scene.skyboxTexKey;

    if (skyboxHasForegroundNodes(scene)) {
        const size_t foreground = countRenderableNodes(scene) - scene.skyboxLayerIds.size();
        LOG_INFO("scene has a skybox plus %zu foreground nodes; foreground renders flat "
                 "(no perspective camera)",
                 foreground);
    }

    rgraph.addPass<SkyboxPass>(
        "skybox",
        rg::PassNode::Type::CustomShader,
        [&pano_key](rg::RenderGraphBuilder& builder, SkyboxPass::Desc& pdesc) {
            pdesc.output       = std::string(SpecTex_Default);
            pdesc.pano_tex_key = pano_key;

            // Read the panorama texture node so the graph orders the upload
            // before this pass, then write _rt_default as the base layer.
            rg::TexNode::Desc in_desc {
                .name = pano_key,
                .key  = pano_key,
                .type = rg::TexNode::TexType::Imported,
            };
            auto* in_node = builder.createTexNode(in_desc);
            builder.read(in_node);

            auto* out_node =
                builder.createTexNode(rg::TexNode::Desc { .name = std::string(SpecTex_Default),
                                                          .key  = std::string(SpecTex_Default),
                                                          .type = rg::TexNode::TexType::Temp },
                                      true);
            builder.write(out_node);
        });
}

} // namespace wallpaper::vulkan

// A self-referencing link entry's "source" and "consumer" are the same layer, so there
// is no ordering of copy-then-read that isn't also an ordering against the producer --
// that's the cycle this exists to avoid.  Bind the layer's own pre-effect capture
// instead; log and leave the texture unset if none exists (no imgeff, or
// copybackground=false skipped the base pass -- see ToGraphPass).
static void bindSelfReferencingLink(rg::RenderGraph& rgraph, ExtraInfo& extra,
                                    const DelayLinkInfo& info) {
    if (! exists(extra.id_preeffect_map, info.link_id)) {
        LOG_ERROR("self-referencing link id=%zu has no pre-effect capture to bind "
                  "(layer has no imgeff, or copybackground=false skipped the base pass); "
                  "leaving the texture unset instead of closing a cycle",
                  info.link_id);
        return;
    }
    auto* pre = extra.id_preeffect_map.at(info.link_id);
    rgraph.afterBuild(
        info.id,
        [pre, tex_index = info.tex_index](rg::RenderGraphBuilder& builder, rg::Pass& rgpass) {
            auto& pass = static_cast<vulkan::CustomShaderPass&>(rgpass);
            builder.read(pre);
            pass.setDescTex((u32)tex_index, pre->key());
            return true;
        });
}

std::unique_ptr<rg::RenderGraph> wallpaper::sceneToRenderGraph(Scene& scene) {
    std::unique_ptr<rg::RenderGraph> rgraph = std::make_unique<rg::RenderGraph>();
    ExtraInfo                        extra { .rgraph = rgraph.get(), .scene = &scene };
    // Every (node id, effect index) the parser registered a runtime
    // visibility binding for, script- or user-property-bound alike.
    // LoadEffectChain looks these up per effect as it walks the scene graph;
    // it reads Scene::scriptEffectVisLayers itself for layers a script
    // toggles effects on.
    for (auto& ps : scene.propertyScripts) {
        if (ps.attachment != ScenePropertyScript::Attachment::Effect) continue;
        extra.hideable_effect_keys.insert(PackNodeEffectKey(ps.id, ps.effectIndex));
    }
    for (auto& [name, bindings] : scene.userPropEffectVisBindings) {
        for (auto& b : bindings) {
            extra.hideable_effect_keys.insert(PackNodeEffectKey(b.nodeId, b.effectIndex));
        }
    }
    {
        int pos = 0;
        for (auto& child : scene.sceneGraph->GetChildren()) {
            LOG_INFO("root child[%d]: id=%d mesh=%s",
                     pos++,
                     child->ID(),
                     child->HasMaterial() ? "yes" : "no");
        }
    }
    // Add reflected-camera passes for planar reflection (_rt_Reflection)
    // BEFORE main passes.  This establishes the _rt_Reflection TexNode chain
    // so that when the grid pass (main) reads _rt_Reflection, it gets the last
    // version written by the reflection passes.  The render graph's dependency
    // resolution then correctly orders: reflection passes → blur → grid pass.
    if (scene.cameras.count("reflected_perspective") > 0) {
        LOG_INFO("adding reflection passes (reflected_perspective camera)");
        TraverseNode(
            [&extra](SceneNode* node) {
                addReflectionPass(node, extra);
            },
            scene.sceneGraph.get());

        // Separable blur on _rt_Reflection to soften the reflection
        if (! scene.reflectionBlurConfig.nodes.empty()) {
            LOG_INFO("adding %zu reflection blur passes", scene.reflectionBlurConfig.nodes.size());
            for (size_t i = 0; i < scene.reflectionBlurConfig.nodes.size(); i++) {
                ToGraphPass(scene.reflectionBlurConfig.nodes[i].get(),
                            scene.reflectionBlurConfig.outputs[i],
                            -1,
                            extra);
            }
        }
    }

    // Equirect skybox background — writes _rt_default first so the scene's own
    // layers composite on top.  No-op unless the scene is skybox-tagged with a
    // resolved panorama (gate inside), so flat scenes stay byte-identical.
    vulkan::emitSkyboxPass(*rgraph, scene);

    TraverseNode(
        [&extra](SceneNode* node) {
            ToGraphPass(node, SpecTex_Default, node->ID(), extra);
        },
        scene.sceneGraph.get());

    LOG_INFO("resolving %zu link textures, id_link_map has %zu entries",
             extra.link_info.size(),
             extra.id_link_map.size());
    for (auto& [id, texnode] : extra.id_link_map) {
        LOG_INFO(
            "  id_link_map[%zu] = '%.*s'", id, (int)texnode->key().size(), texnode->key().data());
    }
    // One snapshot copy per link source, shared by every consumer of it.  Copying
    // per consumer instead would version-bump the same immutable snapshot once per
    // reader, and the ordering constraints between those versions can close into a
    // cycle (Game of Life links one source from two separate effect stages).
    Map<size_t, rg::TexNode*> resolved_link_tex;
    for (auto& info : extra.link_info) {
        if (info.owner_img_id == info.link_id) {
            bindSelfReferencingLink(*rgraph, extra, info);
            continue;
        }
        if (! exists(extra.id_link_map, info.link_id)) {
            LOG_ERROR("link tex %zu not found", info.link_id);
            continue;
        }
        if (! exists(resolved_link_tex, info.link_id)) {
            auto* link_tex_node = extra.id_link_map.at(info.link_id);
            auto  copy_desc     = link_tex_node->genDesc();
            copy_desc.key       = GenLinkTex((idx)info.link_id);
            copy_desc.name      = copy_desc.key;

            resolved_link_tex[info.link_id] = rg::addCopyPass(*rgraph, link_tex_node, &copy_desc);
        }
        auto* new_in = resolved_link_tex.at(info.link_id);
        rgraph->afterBuild(info.id,
                           [new_in, tex_index = info.tex_index](rg::RenderGraphBuilder& builder,
                                                                rg::Pass&               rgpass) {
                               auto& pass = static_cast<vulkan::CustomShaderPass&>(rgpass);
                               builder.read(new_in);
                               pass.setDescTex((u32)tex_index, new_in->key());
                               return true;
                           });
    }

    // Volumetric fog chain — runs between the main scene + link resolution
    // and bloom, so bloom can pick up the scatter contribution.  Hard-skipped
    // when no light wants fog (enabled=false), when per_light is empty (the
    // force-off env path leaves it that way), or in reflection passes (the
    // chain reads the main scene depth, not the reflected one).
    if (scene.activeCamera != nullptr) {
        vulkan::ExtraInfoVolumetrics vol_extra { .scene = &scene, .rgraph = rgraph.get() };
        vulkan::emitVolumetricChain(*rgraph, scene, *scene.activeCamera, vol_extra);
    }

    // Bloom post-processing: copy scene → bloom input, then 4 bloom passes
    if (scene.bloomConfig.enabled && ! scene.bloomConfig.nodes.empty()) {
        LOG_INFO("adding %zu bloom passes", scene.bloomConfig.nodes.size());
        rg::addCopyPass(*rgraph,
                        rg::createTexDesc(std::string(SpecTex_Default)),
                        rg::createTexDesc(std::string(WE_BLOOM_SCENE)));
        for (size_t i = 0; i < scene.bloomConfig.nodes.size(); i++) {
            ToGraphPass(scene.bloomConfig.nodes[i].get(), scene.bloomConfig.outputs[i], -1, extra);
        }
    }

    if (extra.use_mipmap_framebuffer) {
        rg::addCopyPass(*rgraph,
                        rg::TexNode::Desc { .name = SpecTex_Default.data(),
                                            .key  = SpecTex_Default.data(),
                                            .type = rg::TexNode::TexType::Temp },
                        rg::TexNode::Desc { .name = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                            .key  = WE_MIP_MAPPED_FRAME_BUFFER.data(),
                                            .type = rg::TexNode::TexType::Temp });
    }

    return rgraph;
}
