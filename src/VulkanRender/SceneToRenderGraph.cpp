#include "SceneToRenderGraph.hpp"

#include "Scene/Scene.h"
#include "RenderGraph/RenderGraph.hpp"
#include "SpecTexs.hpp"
#include "Utils/Logging.h"
#include "Core/MapSet.hpp"

#include "VulkanRender/AllPasses.hpp"

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
};

struct ExtraInfo {
    Map<size_t, rg::TexNode*>  id_link_map {};
    std::vector<DelayLinkInfo> link_info {};
    rg::RenderGraph*           rgraph { nullptr };
    Scene*                     scene { nullptr };
    bool                       use_mipmap_framebuffer { false };
};

static void ToGraphPass(SceneNode* node, std::string_view output, i32 imgId, ExtraInfo& extra);

static void LoadEffectChain(SceneNode* node, SceneImageEffectLayer* effs, ExtraInfo& extra) {
    auto& rgraph = *extra.rgraph;
    auto& scene  = *extra.scene;

    effs->ResolveEffect(scene.default_effect_mesh, "effect");

    for (usize i = 0; i < effs->EffectCount(); i++) {
        auto& eff     = effs->GetEffect(i);
        auto  cmdItor = eff->commands.begin();
        auto  cmdEnd  = eff->commands.end();
        int   nodePos = 0;
        for (auto& n : eff->nodes) {
            if (cmdItor != cmdEnd && nodePos == cmdItor->afterpos) {
                rg::addCopyPass(
                    rgraph, rg::createTexDesc(cmdItor->src), rg::createTexDesc(cmdItor->dst));
                cmdItor++;
            }
            auto& name = n.output;
            if (n.sceneNode->HasMaterial()) {
                auto& texs = n.sceneNode->Mesh()->Material()->textures;
                std::string tex_list;
                for (usize t = 0; t < texs.size(); t++) {
                    if (t > 0) tex_list += ", ";
                    tex_list += texs[t].empty() ? "(empty)" : texs[t];
                }
                LOG_INFO("  id=%d eff[%zu].%d: out='%.*s' cam='%s' blend=%d texs=[%s]",
                         node->ID(), i, nodePos, (int)name.size(), name.data(),
                         n.sceneNode->Camera().c_str(),
                         (int)n.sceneNode->Mesh()->Material()->blenmode,
                         tex_list.c_str());
            }
            ToGraphPass(n.sceneNode.get(), name, node->ID(), extra);
            nodePos++;
        }
    }
}

static void ToGraphPass(SceneNode* node, std::string_view output, i32 imgId, ExtraInfo& extra) {
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
        auto& cam = scene.cameras.at(node->Camera());
        if (cam->HasImgEffect()) {
            imgeff = cam->GetImgEffect().get();
            output = imgeff->FirstTarget();
        }
    }

    // Passthrough compose layers copy _rt_default → pingpong then run their
    // effect chain.  Process inline at the compose layer's natural Z-order
    // so foreground elements (e.g. Lucy) render ON TOP of the compose result.
    // At this point _rt_default contains background content rendered before
    // this node in the scene graph — exactly what the compose needs.
    if (imgeff != nullptr && imgeff->IsPassthrough()) {
        LOG_INFO("passthrough compose: inline copy _rt_default → '%.*s'",
                 (int)output.size(), output.data());
        rg::addCopyPass(rgraph,
                        rg::createTexDesc(std::string(SpecTex_Default)),
                        rg::createTexDesc(std::string(output)));
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
        // Force Normal blend so the first write uses DONT_CARE load op
        // instead of LOAD on an uninitialized render target (Vulkan UB).
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
        [material, node, &output, &imgId, &rgraph, &scene, &extra](
            rg::RenderGraphBuilder& builder, vulkan::CustomShaderPass::Desc& pdesc) {
            const auto& pass = builder.workPassNode();
            pdesc.node       = node;
            pdesc.output     = output;
            CheckAndSetSprite(scene, pdesc, material->textures);
            for (usize i = 0; i < material->textures.size(); i++) {
                const auto&  url = material->textures[i];
                rg::TexNode* input { nullptr };
                if (url.empty()) {
                    pdesc.textures.emplace_back("");
                    continue;
                } else if (IsSpecLinkTex(url)) {
                    auto id = ParseLinkTex(url);
                    extra.link_info.push_back(
                        DelayLinkInfo { .id = pass.ID(), .link_id = id, .tex_index = (i32)i });
                    pdesc.textures.emplace_back("");
                    continue;
                } else {
                    rg::TexNode::Desc desc;
                    desc.key  = url;
                    desc.name = url;
                    desc.type = ! IsSpecTex(url) ? rg::TexNode::TexType::Imported
                                                 : rg::TexNode::TexType::Temp;
                    input     = builder.createTexNode(desc);
                    if (IsSpecTex(url)) builder.markVirtualWrite(input);
                    if (sstart_with(url, WE_MIP_MAPPED_FRAME_BUFFER))
                        extra.use_mipmap_framebuffer = true;
                }

                if (url == output) {
                    builder.markSelfWrite(input);
                    input = rg::addCopyPass(rgraph, input);
                }
                builder.read(input);
                pdesc.textures.emplace_back(input->key());
            }

            rg::TexNode* output_node { nullptr };
            output_node =
                builder.createTexNode(rg::TexNode::Desc { .name = output.data(),
                                                          .key  = output.data(),
                                                          .type = rg::TexNode::TexType::Temp },
                                      true);
            builder.write(output_node);
            if (output == SpecTex_Default || output == GenOffscreenRT(imgId)) {
                extra.id_link_map[(usize)imgId] = output_node;
            }
        });

    // load effect
    if (imgeff != nullptr) loadEffect(imgeff);
}

// Render a scene node into _rt_Reflection using the reflected camera.
// Skips nodes that read _rt_Reflection (the reflecting surface itself)
// and offscreen/invisible nodes.
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

    // Skip offscreen/invisible nodes
    if (node->IsOffscreen()) return;

    // Skip effect camera nodes (internal compositing passes)
    if (! node->Camera().empty()) {
        auto& cam = scene.cameras.at(node->Camera());
        if (cam->HasImgEffect()) return;
    }

    std::string passName = "refl_" + material->name;

    rgraph.addPass<vulkan::CustomShaderPass>(
        passName,
        rg::PassNode::Type::CustomShader,
        [material, node, &scene, &extra](
            rg::RenderGraphBuilder& builder, vulkan::CustomShaderPass::Desc& pdesc) {
            pdesc.node            = node;
            pdesc.output          = std::string(WE_REFLECTION);
            pdesc.camera_override = "reflected_perspective";
            pdesc.disableDepth        = false;
            pdesc.flipCullMode        = false;  // col(1) and row(1) negate cancel winding
            pdesc.useReflectionDepth  = true;
            CheckAndSetSprite(scene, pdesc, material->textures);

            for (usize i = 0; i < material->textures.size(); i++) {
                const auto& url = material->textures[i];
                if (url.empty()) {
                    pdesc.textures.emplace_back("");
                    continue;
                }
                if (IsSpecLinkTex(url)) {
                    pdesc.textures.emplace_back("");
                    continue;
                }

                rg::TexNode::Desc desc;
                desc.key  = url;
                desc.name = url;
                desc.type = ! IsSpecTex(url) ? rg::TexNode::TexType::Imported
                                             : rg::TexNode::TexType::Temp;
                auto* input = builder.createTexNode(desc);
                if (IsSpecTex(url)) builder.markVirtualWrite(input);
                builder.read(input);
                pdesc.textures.emplace_back(input->key());
            }

            auto* output_node =
                builder.createTexNode(rg::TexNode::Desc { .name = std::string(WE_REFLECTION),
                                                           .key  = std::string(WE_REFLECTION),
                                                           .type = rg::TexNode::TexType::Temp },
                                      true);
            builder.write(output_node);
        });
}

std::unique_ptr<rg::RenderGraph> wallpaper::sceneToRenderGraph(Scene& scene) {
    std::unique_ptr<rg::RenderGraph> rgraph = std::make_unique<rg::RenderGraph>();
    ExtraInfo                        extra { .rgraph = rgraph.get(), .scene = &scene };
    {
        int pos = 0;
        for (auto& child : scene.sceneGraph->GetChildren()) {
            LOG_INFO("root child[%d]: id=%d mesh=%s", pos++, child->ID(),
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
            LOG_INFO("adding %zu reflection blur passes",
                     scene.reflectionBlurConfig.nodes.size());
            for (size_t i = 0; i < scene.reflectionBlurConfig.nodes.size(); i++) {
                ToGraphPass(scene.reflectionBlurConfig.nodes[i].get(),
                            scene.reflectionBlurConfig.outputs[i], -1, extra);
            }
        }
    }

    TraverseNode(
        [&extra](SceneNode* node) {
            ToGraphPass(node, SpecTex_Default, node->ID(), extra);
        },
        scene.sceneGraph.get());

    LOG_INFO("resolving %zu link textures, id_link_map has %zu entries",
             extra.link_info.size(), extra.id_link_map.size());
    for (auto& [id, texnode] : extra.id_link_map) {
        LOG_INFO("  id_link_map[%zu] = '%.*s'", id, (int)texnode->key().size(), texnode->key().data());
    }
    for (auto& info : extra.link_info) {
        if (! exists(extra.id_link_map, info.link_id)) {
            LOG_ERROR("link tex %d not found", info.link_id);
            continue;
        }
        rgraph->afterBuild(
            info.id, [&rgraph, &extra, &info](rg::RenderGraphBuilder& builder, rg::Pass& rgpass) {
                auto& pass = static_cast<vulkan::CustomShaderPass&>(rgpass);

                auto* link_tex_node = extra.id_link_map.at(info.link_id);
                auto  copy_desc     = link_tex_node->genDesc();
                copy_desc.key       = GenLinkTex((idx)info.link_id);
                copy_desc.name      = copy_desc.key;

                auto new_in = rg::addCopyPass(*rgraph, link_tex_node, &copy_desc);
                builder.read(new_in);
                pass.setDescTex((u32)info.tex_index, new_in->key());
                return true;
            });
    }

    // Bloom post-processing: copy scene → bloom input, then 4 bloom passes
    if (scene.bloomConfig.enabled && ! scene.bloomConfig.nodes.empty()) {
        LOG_INFO("adding %zu bloom passes", scene.bloomConfig.nodes.size());
        rg::addCopyPass(*rgraph,
                        rg::createTexDesc(std::string(SpecTex_Default)),
                        rg::createTexDesc(std::string(WE_BLOOM_SCENE)));
        for (size_t i = 0; i < scene.bloomConfig.nodes.size(); i++) {
            ToGraphPass(scene.bloomConfig.nodes[i].get(),
                        scene.bloomConfig.outputs[i], -1, extra);
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
