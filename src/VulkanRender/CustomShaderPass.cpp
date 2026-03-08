#include "CustomShaderPass.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneShader.h"

#include "SpecTexs.hpp"
#include "Vulkan/Shader.hpp"
#include "Utils/Logging.h"
#include "Utils/AutoDeletor.hpp"
#include "Resource.hpp"
#include "PassCommon.hpp"
#include "Interface/IImageParser.h"

#include "Core/ArrayHelper.hpp"

#include <cassert>

using namespace wallpaper::vulkan;

// Frame-level pass execution counters (reset in VulkanRender.cpp each frame)
namespace wallpaper::vulkan {
    int g_exec_pass_counter = 0;
    int g_exec_frame_counter = 0;
    bool g_depth_transitioned = false;            // reset each frame in VulkanRender.cpp
    bool g_refl_depth_transitioned = false;       // reset each frame in VulkanRender.cpp
}

CustomShaderPass::CustomShaderPass(const Desc& desc) {
    m_desc.node            = desc.node;
    m_desc.textures        = desc.textures;
    m_desc.output          = desc.output;
    m_desc.sprites_map     = desc.sprites_map;
    m_desc.camera_override = desc.camera_override;
    m_desc.disableDepth        = desc.disableDepth;
    m_desc.flipCullMode        = desc.flipCullMode;
    m_desc.useReflectionDepth  = desc.useReflectionDepth;
};
CustomShaderPass::~CustomShaderPass() {}

constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

static std::shared_ptr<VmaImageParameters>
GetOrCreateDepthImage(std::shared_ptr<void>& storage, const Device& device, VkExtent3D extent) {
    auto existing = std::static_pointer_cast<VmaImageParameters>(storage);
    if (existing && existing->extent.width == extent.width &&
        existing->extent.height == extent.height) {
        return existing;
    }

    auto depth = std::make_shared<VmaImageParameters>();
    VkImageCreateInfo info {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext       = nullptr,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = DEPTH_FORMAT,
        .extent      = extent,
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .tiling      = VK_IMAGE_TILING_OPTIMAL,
        .usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo vma_info {};
    vma_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vvk::CreateImage(device.vma_allocator(), info, vma_info, depth->handle) != VK_SUCCESS) {
        LOG_ERROR("depth image creation failed");
        return nullptr;
    }
    depth->extent = extent;
    VkImageViewCreateInfo view_info {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext    = nullptr,
        .image    = *depth->handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = DEPTH_FORMAT,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                              .baseMipLevel = 0, .levelCount = 1,
                              .baseArrayLayer = 0, .layerCount = 1 },
    };
    if (device.handle().CreateImageView(view_info, depth->view) != VK_SUCCESS) {
        LOG_ERROR("depth image view creation failed");
        return nullptr;
    }
    storage = depth;
    LOG_INFO("created depth buffer %dx%d", extent.width, extent.height);
    return depth;
}

std::optional<vvk::RenderPass> CreateRenderPass(const vvk::Device& device, VkFormat format,
                                                VkAttachmentLoadOp loadOp,
                                                VkImageLayout      finalLayout,
                                                bool               hasDepth = false,
                                                VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR) {
    VkAttachmentDescription attachments[2];
    attachments[0] = VkAttachmentDescription {
        .format         = format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = loadOp,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = finalLayout,
    };

    if (loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    uint32_t attachmentCount = 1;
    VkAttachmentReference depth_ref {};

    if (hasDepth) {
        // Always use DEPTH_STENCIL_ATTACHMENT_OPTIMAL as initialLayout.
        // An explicit barrier in execute() transitions the depth image from
        // UNDEFINED to DEPTH_STENCIL_ATTACHMENT_OPTIMAL before first use.
        attachments[1] = VkAttachmentDescription {
            .format         = DEPTH_FORMAT,
            .samples        = VK_SAMPLE_COUNT_1_BIT,
            .loadOp         = depthLoadOp,
            .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        depth_ref = VkAttachmentReference {
            .attachment = 1,
            .layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        attachmentCount = 2;
    }

    VkAttachmentReference color_ref {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass {
        .pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount    = 1,
        .pColorAttachments       = &color_ref,
        .pDepthStencilAttachment = hasDepth ? &depth_ref : nullptr,
    };

    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkAccessFlags        dstAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (hasDepth) {
        dstStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dstAccess |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                     VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    VkSubpassDependency dependency {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .dstStageMask  = dstStage,
        .srcAccessMask = {},
        .dstAccessMask = dstAccess,
    };

    VkRenderPassCreateInfo creatinfo {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = attachmentCount,
        .pAttachments    = attachments,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dependency,
    };
    vvk::RenderPass pass;
    if (auto res = device.CreateRenderPass(creatinfo, pass); res == VK_SUCCESS) {
        return pass;
    } else {
        VVK_CHECK(res);
        return std::nullopt;
    }
}

static void UpdateUniform(StagingBuffer* buf, const StagingBufferRef& bufref,
                          const ShaderReflected::Block& block, std::string_view name,
                          const wallpaper::ShaderValue& value) {
    using namespace wallpaper;
    std::span<uint8_t> value_u8 { (uint8_t*)value.data(),
                                  value.size() * sizeof(ShaderValue::value_type) };
    auto               uni = block.member_map.find(name);
    if (uni == block.member_map.end()) {
        return;
    }

    size_t offset    = uni->second.offset;
    // Use SPIR-V member size when available, fall back to num-based calculation
    size_t type_size = uni->second.size > 0 ? uni->second.size
                                            : sizeof(float) * uni->second.num;
    // Clamp write to member size to prevent overflow into adjacent UBO members
    // (e.g., mat4 ShaderValue written to mat3 uniform)
    if (value_u8.size() > type_size) {
        value_u8 = value_u8.subspan(0, type_size);
    }
    buf->writeToBuf(bufref, value_u8, offset);
}

void CustomShaderPass::prepare(Scene& scene, const Device& device, RenderingResources& rr) {
    m_desc.vk_textures.resize(m_desc.textures.size());
    for (usize i = 0; i < m_desc.textures.size(); i++) {
        auto& tex_name = m_desc.textures[i];
        if (tex_name.empty()) continue;

        ImageSlotsRef img_slots;
        if (IsSpecTex(tex_name)) {
            if (scene.renderTargets.count(tex_name) == 0) continue;
            auto& rt  = scene.renderTargets.at(tex_name);
            auto  opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
            if (! opt.has_value()) continue;
            img_slots.slots = { opt.value() };
        } else {
            auto image = scene.imageParser->Parse(tex_name);
            if (image) {
                img_slots = device.tex_cache().CreateTex(*image);
            } else {
                LOG_ERROR("parse tex \"%s\" failed", tex_name.c_str());
            }
        }
        m_desc.vk_textures[i] = img_slots;
    }
    {
        auto& tex_name = m_desc.output;
        assert(IsSpecTex(tex_name));
        assert(scene.renderTargets.count(tex_name) > 0);
        auto& rt = scene.renderTargets.at(tex_name);
        if (auto opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
            opt.has_value()) {
            m_desc.vk_output = opt.value();
        } else {
            LOG_ERROR("output RT query failed for '%s' (shader: %s)",
                      tex_name.c_str(),
                      m_desc.node->Mesh()->Material()->customShader.shader->name.c_str());
            return;
        }
    }

    SceneMesh& mesh = *(m_desc.node->Mesh());

    std::vector<Uni_ShaderSpv> spvs;
    DescriptorSetInfo          descriptor_info;
    ShaderReflected            ref;
    {
        SceneShader& shader = *(mesh.Material()->customShader.shader);

        if (! GenReflect(shader.codes, spvs, ref)) {
            LOG_ERROR("gen spv reflect failed, %s", shader.name.c_str());
            return;
        }

        // Check if shader uses time-based uniforms (for caching optimization)
        if (!ref.blocks.empty()) {
            auto& block = ref.blocks.front();
            m_uses_time_uniforms = exists(block.member_map, G_TIME) ||
                                   exists(block.member_map, G_DAYTIME) ||
                                   exists(block.member_map, G_POINTERPOSITION);
        }

        auto& bindings = descriptor_info.bindings;
        bindings.resize(ref.binding_map.size());

        /*
        LOG_INFO("----shader------");
        LOG_INFO("%s", shader.name.c_str());
        LOG_INFO("--inputs:");
        for (auto& i : ref.input_location_map) {
            LOG_INFO("%d %s", i.second, i.first.c_str());
        }
        LOG_INFO("--bindings:");
        */

        std::transform(
            ref.binding_map.begin(), ref.binding_map.end(), bindings.begin(), [](auto& item) {
                // LOG_INFO("%d %s", item.second.binding, item.first.c_str());
                return item.second;
            });

        for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
            i32 binding { -1 };
            if (exists(ref.binding_map, WE_GLTEX_NAMES[i]))
                binding = (i32)ref.binding_map.at(WE_GLTEX_NAMES[i]).binding;
            m_desc.vk_tex_binding.push_back(binding);
        }
    }

    m_desc.draw_count = 0;
    std::vector<VkVertexInputBindingDescription>   bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
    {
        m_desc.dyn_vertex = mesh.Dynamic();
        m_desc.vertex_bufs.resize(mesh.VertexCount());

        for (uint i = 0; i < mesh.VertexCount(); i++) {
            const auto& vertex    = mesh.GetVertexArray(i);
            auto        attrs_map = vertex.GetAttrOffsetMap();

            VkVertexInputBindingDescription bind_desc {
                .binding   = i,
                .stride    = (uint32_t)vertex.OneSizeOf(),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };
            bind_descriptions.push_back(bind_desc);

            for (auto& item : ref.input_location_map) {
                auto& name   = item.first;
                auto& input  = item.second;
                usize offset = exists(attrs_map, name) ? attrs_map[name].offset : 0;

                VkVertexInputAttributeDescription attr_desc {
                    .location = input.location,
                    .binding  = i,
                    .format   = input.format,
                    .offset   = (u32)offset,
                };
                attr_descriptions.push_back(attr_desc);
            }
            {
                auto& buf = m_desc.vertex_bufs[i];
                if (! m_desc.dyn_vertex) {
                    if (! rr.vertex_buf->allocateSubRef(vertex.CapacitySizeOf(), buf)) {
                        LOG_ERROR("vertex buf alloc failed for shader '%s' vert %u, size %zu",
                                  mesh.Material()->customShader.shader->name.c_str(), i, vertex.CapacitySizeOf());
                        return;
                    }
                    if (! rr.vertex_buf->writeToBuf(buf, { (uint8_t*)vertex.Data(), buf.size })) {
                        LOG_ERROR("vertex buf write failed for shader '%s'",
                                  mesh.Material()->customShader.shader->name.c_str());
                        return;
                    }
                } else {
                    if (! rr.dyn_buf->allocateSubRef(vertex.CapacitySizeOf(), buf)) {
                        LOG_ERROR("dyn buf alloc failed for shader '%s'",
                                  mesh.Material()->customShader.shader->name.c_str());
                        return;
                    }
                }
            }
            m_desc.draw_count += (u32)(vertex.DataSize() / vertex.OneSize());
        }

        if (mesh.IndexCount() > 0) {
            auto&  indice     = mesh.GetIndexArray(0);
            size_t count      = (indice.DataCount() * 2) / 3;
            m_desc.draw_count = (u32)count * 3;
            auto& buf         = m_desc.index_buf;
            if (! m_desc.dyn_vertex) {
                if (! rr.vertex_buf->allocateSubRef(indice.CapacitySizeof(), buf)) {
                    LOG_ERROR("index buf alloc failed for shader '%s'",
                              mesh.Material()->customShader.shader->name.c_str());
                    return;
                }
                if (! rr.vertex_buf->writeToBuf(buf, { (uint8_t*)indice.Data(), buf.size })) {
                    LOG_ERROR("index buf write failed for shader '%s'",
                              mesh.Material()->customShader.shader->name.c_str());
                    return;
                }
            } else {
                if (! rr.dyn_buf->allocateSubRef(indice.CapacitySizeof(), buf)) {
                    LOG_ERROR("dyn index buf alloc failed for shader '%s'",
                              mesh.Material()->customShader.shader->name.c_str());
                    return;
                }
            }
        }
    }
    {
        VkPipelineColorBlendAttachmentState color_blend;
        VkAttachmentLoadOp                  loadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        {
            VkColorComponentFlags colorMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            bool alpha =
                ! (m_desc.node->Camera().empty() || sstart_with(m_desc.node->Camera(), "global"));
            // Non-default RTs (offscreen, pingpong, composite) need alpha written
            // so downstream blend effects can read meaningful alpha values.
            bool non_default_rt = (m_desc.output != SpecTex_Default);

            if (alpha || non_default_rt) colorMask |= VK_COLOR_COMPONENT_A_BIT;
            color_blend.colorWriteMask = colorMask;

            auto blendmode = mesh.Material()->blenmode;
            SetBlend(blendmode, color_blend);
            m_desc.blending = color_blend.blendEnable;

            // Non-default RTs: first write CLEARs (initialize to transparent black),
            // subsequent writes LOAD (accumulate content). Mirrors depthBufferCleared
            // pattern. Default RT uses blend-mode-based load ops as before.
            if (non_default_rt) {
                if (scene.clearedRTs.count(m_desc.output) == 0) {
                    loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    scene.clearedRTs.insert(m_desc.output);
                } else {
                    loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                }
            } else {
                SetAttachmentLoadOp(blendmode, loadOp);
            }

        }
        // Depth buffer for 3D models
        bool useDepth = !m_desc.disableDepth &&
                        (mesh.Material()->depthTest || mesh.Material()->depthWrite);
        std::shared_ptr<VmaImageParameters> depthImg;
        if (useDepth) {
            VkExtent3D depthExtent { m_desc.vk_output.extent.width,
                                     m_desc.vk_output.extent.height, 1 };
            auto& depthStorage = m_desc.useReflectionDepth
                                     ? scene.reflectionDepthBuffer
                                     : scene.depthBuffer;
            depthImg = GetOrCreateDepthImage(depthStorage, device, depthExtent);
            if (! depthImg) {
                LOG_ERROR("depth buffer creation failed for shader '%s'",
                          mesh.Material()->customShader.shader->name.c_str());
                useDepth = false;
            }
        }

        // Depth buffer is cleared explicitly via vkCmdClearDepthStencilImage
        // before the first depth pass each frame (in execute()).  All render
        // passes use LOAD to preserve the cleared/written values.
        VkAttachmentLoadOp depthLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        scene.depthBufferCleared = true;
        auto opt = CreateRenderPass(device.handle(),
                                    VK_FORMAT_R8G8B8A8_UNORM,
                                    loadOp,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    useDepth,
                                    depthLoadOp);
        if (! opt.has_value()) {
            LOG_ERROR("CreateRenderPass failed for shader '%s'",
                      mesh.Material()->customShader.shader->name.c_str());
            return;
        }
        auto& pass = opt.value();
        m_desc.hasDepth  = useDepth;
        m_desc.depthView = useDepth ? *depthImg->view : VK_NULL_HANDLE;
        m_desc.depthImage = useDepth ? *depthImg->handle : VK_NULL_HANDLE;

        descriptor_info.push_descriptor = true;
        GraphicsPipeline pipeline;
        pipeline.toDefault();
        if (mesh.Material()->depthTest) {
            pipeline.depth.depthTestEnable  = VK_TRUE;
            pipeline.depth.depthCompareOp   = VK_COMPARE_OP_LESS;
        }
        if (mesh.Material()->depthBiasConstant != 0) {
            pipeline.raster.depthBiasEnable         = VK_TRUE;
            pipeline.raster.depthBiasConstantFactor  = mesh.Material()->depthBiasConstant;
        }
        if (mesh.Material()->depthWrite) {
            pipeline.depth.depthWriteEnable = VK_TRUE;
        }
        if (mesh.Material()->cullmode == "back") {
            pipeline.raster.cullMode = m_desc.flipCullMode ? VK_CULL_MODE_FRONT_BIT
                                                           : VK_CULL_MODE_BACK_BIT;
        } else if (mesh.Material()->cullmode == "front") {
            pipeline.raster.cullMode = m_desc.flipCullMode ? VK_CULL_MODE_BACK_BIT
                                                           : VK_CULL_MODE_FRONT_BIT;
        }
        pipeline.addDescriptorSetInfo(spanone { descriptor_info })
            .setColorBlendStates(spanone { color_blend })
            .setTopology(m_desc.index_buf ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                          : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
            .addInputBindingDescription(bind_descriptions)
            .addInputAttributeDescription(attr_descriptions);
        for (auto& spv : spvs) pipeline.addStage(std::move(spv));

        if (! pipeline.create(device, pass, m_desc.pipeline)) {
            LOG_ERROR("pipeline.create failed for shader '%s'",
                      mesh.Material()->customShader.shader->name.c_str());
            return;
        }

        // One-time diagnostic per shader: pipeline state for 3D debugging
        {
            static std::set<std::string> _pipe_logged;
            auto key = mesh.Material()->customShader.shader->name + "_" + m_desc.output;
            if (_pipe_logged.insert(key).second) {
                LOG_INFO("pipeline: shader='%s' out='%.*s' draw=%u indexed=%d "
                         "depthTest=%d depthWrite=%d cull=%s blend=%d",
                         mesh.Material()->customShader.shader->name.c_str(),
                         (int)m_desc.output.size(), m_desc.output.data(),
                         m_desc.draw_count, m_desc.index_buf ? 1 : 0,
                         (int)mesh.Material()->depthTest, (int)mesh.Material()->depthWrite,
                         mesh.Material()->cullmode.c_str(),
                         (int)mesh.Material()->blenmode);
            }
        }
    }

    {
        VkImageView fb_attachments[2] = { m_desc.vk_output.view, m_desc.depthView };
        VkFramebufferCreateInfo info {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext           = nullptr,
            .renderPass      = *m_desc.pipeline.pass,
            .attachmentCount = m_desc.hasDepth ? 2u : 1u,
            .pAttachments    = fb_attachments,
            .width           = m_desc.vk_output.extent.width,
            .height          = m_desc.vk_output.extent.height,
            .layers          = 1,
        };
        VVK_CHECK_VOID_RE(device.handle().CreateFramebuffer(info, m_desc.fb));
    }

    if (! ref.blocks.empty()) {
        auto& block = ref.blocks.front();
        if (! rr.dyn_buf->allocateSubRef(
                block.size, m_desc.ubo_buf, device.limits().minUniformBufferOffsetAlignment)) {
            LOG_ERROR("allocate UBO failed, size %zu", (size_t)block.size);
            return;
        }
    }

    if (! ref.blocks.empty()) {
        std::function<void()> update_dyn_buf_op;
        if (m_desc.dyn_vertex) {
            auto& mesh        = *m_desc.node->Mesh();
            auto* dyn_buf     = rr.dyn_buf;
            auto& vertex_bufs = m_desc.vertex_bufs;
            auto& draw_count  = m_desc.draw_count;
            auto& index_buf   = m_desc.index_buf;
            update_dyn_buf_op = [&mesh, &vertex_bufs, &draw_count, &index_buf, dyn_buf]() {
                if (mesh.Dirty().exchange(false)) {
                    for (usize i = 0; i < mesh.VertexCount(); i++) {
                        const auto& vertex = mesh.GetVertexArray(i);
                        auto&       buf    = vertex_bufs[i];
                        if (! dyn_buf->writeToBuf(buf,
                                                  { (uint8_t*)vertex.Data(), vertex.DataSizeOf() }))
                            return;
                    }
                    if (mesh.IndexCount() > 0) {
                        auto& indice = mesh.GetIndexArray(0);
                        u32   count  = (u32)((indice.RenderDataCount() * 2) / 3);
                        draw_count   = count * 3;
                        auto& buf = index_buf;
                        if (! dyn_buf->writeToBuf(buf,
                                                  { (uint8_t*)indice.Data(), indice.DataSizeOf() }))
                            return;
                    }
                }
            };
        }

        auto  block  = ref.blocks.front();
        auto* buf    = rr.dyn_buf;
        auto* bufref = &m_desc.ubo_buf;

        auto* node           = m_desc.node;
        auto* shader_updater = scene.shaderValueUpdater.get();
        auto& sprites        = m_desc.sprites_map;
        auto& vk_textures    = m_desc.vk_textures;
        auto  cam_override   = m_desc.camera_override;

        auto* p_material = &mesh.Material()->customShader;
        m_desc.update_op = [shader_updater,
                            block,
                            buf,
                            bufref,
                            node,
                            p_material,
                            &sprites,
                            &vk_textures,
                            cam_override,
                            update_dyn_buf_op]() {
            auto update_unf_op = [&block, buf, bufref](std::string_view       name,
                                                       wallpaper::ShaderValue value) {
                UpdateUniform(buf, *bufref, block, name, value);
            };
            // Re-upload constValues if they were modified at runtime (user property change)
            if (p_material->constValuesDirty) {
                for (auto& v : p_material->constValues) {
                    if (exists(block.member_map, v.first)) {
                        UpdateUniform(buf, *bufref, block, v.first, v.second);
                    }
                }
                p_material->constValuesDirty = false;
            }
            shader_updater->UpdateUniforms(node, sprites, update_unf_op, cam_override);
            // update image slot for sprites
            {
                for (auto& [i, sp] : sprites) {
                    if (i >= vk_textures.size()) continue;
                    vk_textures.at(i).active = sp.GetCurFrame().imageId;
                }
            }
            if (update_dyn_buf_op) update_dyn_buf_op();
        };

        auto exists_unf_op = [&block](std::string_view name) {
            return exists(block.member_map, name);
        };
        shader_updater->InitUniforms(node, exists_unf_op);

        // memset uniform buf
        buf->fillBuf(*bufref, 0, bufref->size, 0);
        {
            auto&      default_values = mesh.Material()->customShader.shader->default_uniforms;
            auto&      const_values   = mesh.Material()->customShader.constValues;
            std::array values_array   = { &default_values, &const_values };
            for (auto& values : values_array) {
                for (auto& v : *values) {
                    if (exists(block.member_map, v.first)) {
                        UpdateUniform(buf, *bufref, block, v.first, v.second);
                    }
                }
            }
        }
        m_desc.update_op();
    }

    {
        // Non-default RTs clear to transparent so non-content areas don't
        // affect downstream blend effects (blendAlpha *= blendColors.a).
        bool non_default_rt = (m_desc.output != SpecTex_Default);
        if (non_default_rt) {
            m_desc.clear_value = VkClearValue { .color = { 0.0f, 0.0f, 0.0f, 0.0f } };
        } else {
            auto& sc           = scene.clearColor;
            m_desc.clear_value = VkClearValue {
                .color = { sc[0], sc[1], sc[2], 1.0f },
            };
        }
    }
    for (auto& tex : releaseTexs()) {
        device.tex_cache().MarkShareReady(tex);
    }
    setPrepared();
}

void CustomShaderPass::execute(const Device&, RenderingResources& rr) {
    // NOTE: Pass caching disabled - output textures are not preserved between frames
    // in current render graph implementation. Would need persistent render targets.
    // if (isCacheable() && m_cached) { return; }

    if (m_desc.update_op) m_desc.update_op();

    // First-frame execution trace: log every pass with order, shader, output, load op, textures
    {
        extern int g_exec_pass_counter;
        extern int g_exec_frame_counter;
        if (g_exec_frame_counter < 1) {
            auto shaderName = m_desc.node && m_desc.node->Mesh() && m_desc.node->Mesh()->Material()
                ? m_desc.node->Mesh()->Material()->customShader.shader->name : "???";
            bool nodeVisible = (m_desc.node == nullptr || m_desc.node->IsVisible());
            LOG_INFO("EXEC[%d] pass#%d shader='%s' out='%.*s' draw=%u visible=%d "
                     "depth=%d blend=%d tex_count=%zu out_img=%p out_ext=%ux%u",
                     g_exec_frame_counter, g_exec_pass_counter, shaderName.c_str(),
                     (int)m_desc.output.size(), m_desc.output.data(),
                     m_desc.draw_count, (int)nodeVisible,
                     (int)m_desc.hasDepth, (int)m_desc.blending,
                     m_desc.vk_textures.size(),
                     (void*)m_desc.vk_output.handle,
                     m_desc.vk_output.extent.width, m_desc.vk_output.extent.height);
            // Log bound textures
            for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
                auto& slot = m_desc.vk_textures[i];
                int binding = m_desc.vk_tex_binding[i];
                if (binding < 0 || slot.slots.empty()) continue;
                auto& img = slot.getActive();
                LOG_INFO("  tex[%zu] binding=%d extent=%ux%u handle=%p",
                         i, binding, img.extent.width, img.extent.height, (void*)img.handle);
            }
            g_exec_pass_counter++;
        }
    }

    auto&                   cmd    = rr.command;
    auto&                   outext = m_desc.vk_output.extent;

    // Explicitly clear depth buffer to 1.0 on first use each frame.
    // Using vkCmdClearDepthStencilImage instead of render pass loadOp=CLEAR
    // to work around drivers where the render pass CLEAR doesn't execute
    // correctly with a newly-created depth image (RADV GFX1201).
    bool& depth_flag = m_desc.useReflectionDepth ? g_refl_depth_transitioned
                                                   : g_depth_transitioned;
    if (m_desc.hasDepth && !depth_flag) {
        VkImageSubresourceRange depth_range {
            .aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        };

        // Transition UNDEFINED → TRANSFER_DST for the clear command
        VkImageMemoryBarrier to_transfer {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = 0,
            .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = m_desc.depthImage,
            .subresourceRange    = depth_range,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, to_transfer);

        // Explicit clear to 1.0
        VkClearDepthStencilValue clear_depth { 1.0f, 0 };
        cmd.ClearDepthStencilImage(m_desc.depthImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   clear_depth, depth_range);

        // Transition TRANSFER_DST → DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        VkImageMemoryBarrier to_attach {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext               = nullptr,
            .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask       = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout           = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = m_desc.depthImage,
            .subresourceRange    = depth_range,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                            0, to_attach);
        depth_flag = true;
    }

    VkImageSubresourceRange base_srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_ARRAY_LAYERS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_MIP_LEVELS,
    };
    // Batch image barriers for better performance
    std::vector<VkImageMemoryBarrier> image_barriers;
    image_barriers.reserve(m_desc.vk_textures.size());

    for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
        auto& slot    = m_desc.vk_textures[i];
        int   binding = m_desc.vk_tex_binding[i];
        if (binding < 0) continue;
        if (slot.slots.empty()) continue;
        auto&                 img = slot.getActive();
        VkDescriptorImageInfo desc_img { img.sampler,
                                         img.view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet  wset {
             .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .pNext           = nullptr,
             .dstSet          = {},
             .dstBinding      = (uint32_t)binding,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo      = &desc_img,
        };
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wset);

        image_barriers.push_back(VkImageMemoryBarrier {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image            = img.handle,
            .subresourceRange = base_srang,
        });
    }

    // Single batched barrier call instead of one per texture
    if (!image_barriers.empty()) {
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            {}, {}, image_barriers);
    }

    if (m_desc.ubo_buf) {
        VkDescriptorBufferInfo desc_buf {
            rr.dyn_buf->gpuBuf(),
            m_desc.ubo_buf.offset,
            m_desc.ubo_buf.size,
        };
        VkWriteDescriptorSet wset {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext           = nullptr,
            .dstSet          = {},
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &desc_buf,
        };
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wset);
    }

    VkClearValue clear_values[2];
    clear_values[0] = m_desc.clear_value;
    clear_values[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo pass_begin_info {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext       = nullptr,
        .renderPass  = *m_desc.pipeline.pass,
        .framebuffer = *m_desc.fb,
        .renderArea =
            VkRect2D {
                .offset = { 0, 0 },
                .extent = { outext.width, outext.height },
            },
        .clearValueCount = m_desc.hasDepth ? 2u : 1u,
        .pClearValues    = clear_values,
    };
    cmd.BeginRenderPass(pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.handle);
    VkViewport viewport {
        .x        = 0,
        .y        = (float)outext.height,
        .width    = (float)outext.width,
        .height   = -(float)outext.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor { { 0, 0 }, { outext.width, outext.height } };

    cmd.SetViewport(0, viewport);
    cmd.SetScissor(0, scissor);

    auto gpu_buf = m_desc.dyn_vertex ? rr.dyn_buf->gpuBuf() : rr.vertex_buf->gpuBuf();

    // Skip draw (but keep render pass begin/end) if node is hidden at runtime
    bool nodeVisible = (m_desc.node == nullptr || m_desc.node->IsVisible());
    if (nodeVisible) {
        for (usize i = 0; i < m_desc.vertex_bufs.size(); i++) {
            auto& buf = m_desc.vertex_bufs[i];
            cmd.BindVertexBuffers((u32)i, 1, &gpu_buf, &buf.offset);
        }
        if (m_desc.index_buf) {
            cmd.BindIndexBuffer(gpu_buf, m_desc.index_buf.offset, VK_INDEX_TYPE_UINT16);
            cmd.DrawIndexed(m_desc.draw_count, 1, 0, 0, 0);
        } else {
            cmd.Draw(m_desc.draw_count, 1, 0, 0);
        }
    }

    cmd.EndRenderPass();
}

void CustomShaderPass::destory(const Device&, RenderingResources& rr) {
    m_desc.update_op = {};
    {
        auto& buf = m_desc.dyn_vertex ? rr.dyn_buf : rr.vertex_buf;
        for (auto& bufref : m_desc.vertex_bufs) {
            if (bufref) buf->unallocateSubRef(bufref);
        }
        if (m_desc.index_buf) buf->unallocateSubRef(m_desc.index_buf);
    }
    if (m_desc.ubo_buf) rr.dyn_buf->unallocateSubRef(m_desc.ubo_buf);
}

void CustomShaderPass::setDescTex(u32 index, std::string_view tex_key) {
    assert(index < m_desc.textures.size());
    if (index >= m_desc.textures.size()) return;
    m_desc.textures[index] = tex_key;
}
