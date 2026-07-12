#include "SkyboxPass.hpp"
#include "Vulkan/Shader.hpp"
#include "Resource.hpp"
#include "PassCommon.hpp"
#include "Interface/IImageParser.h"
#include "Utils/Logging.h"
#include "Utils/SceneProfiler.h"

using namespace wallpaper::vulkan;

// Fullscreen quad.  Position is clip-space NDC; the vertex shader reconstructs a
// world-space view direction from it via invViewProj.  Texcoord carries the raw
// NDC so the reconstruction has the [-1,1] range regardless of the y-flip the
// viewport applies.
constexpr std::string_view vert_code = R"(#version 320 es
layout(location = 0) in vec3 Position;
layout(location = 1) in vec2 Texcoord;
layout(location = 0) out vec3 v_WorldDir;

layout(binding = 0) uniform UBO {
    mat4 invViewProj;
    float yawRad;
    float _pad0;
    float _pad1;
    float _pad2;
} ubo;

void main()
{
    gl_Position = vec4(Position, 1.0);

    // Reconstruct a world direction from the NDC position on the far plane.
    // For Increment A invViewProj is identity, so this is a static background;
    // a later increment feeds the scene camera's inverse view-projection.
    vec4 farPoint = ubo.invViewProj * vec4(Position.xy, 1.0, 1.0);
    vec3 dir = farPoint.xyz / farPoint.w;

    // Constant yaw spin about the world up axis (yawRad = 0 in Increment A).
    float s = sin(ubo.yawRad);
    float c = cos(ubo.yawRad);
    mat3 yaw = mat3(c, 0.0, -s,
                    0.0, 1.0, 0.0,
                    s, 0.0, c);
    v_WorldDir = yaw * dir;
}
)";

// Equirectangular sampling: mirror of SkyboxMath.hpp equirectUV so the
// device-free unit tests pin the exact math the GPU runs.
//   u = atan(dir.x, dir.z) / (2π) + 0.5
//   v = acos(clamp(dir.y, -1, 1)) / π
constexpr std::string_view frag_code = R"(#version 320 es
precision highp float;
layout(location = 0) in vec3 v_WorldDir;
layout(location = 0) out vec4 out_FragColor;

layout(binding = 1) uniform sampler2D u_Pano;

const float PI = 3.14159265358979323846;

void main()
{
    vec3 dir = normalize(v_WorldDir);
    float u = atan(dir.x, dir.z) * (1.0 / (2.0 * PI)) + 0.5;
    float v = acos(clamp(dir.y, -1.0, 1.0)) * (1.0 / PI);
    out_FragColor = vec4(texture(u_Pano, vec2(u, v)).rgb, 1.0);
}
)";

// UBO layout — std140 mat4 (64B) + 4 floats (16B) = 80B.  yawRad is 0 for
// Increment A; the pad rounds the block to a 16B boundary.
struct SkyboxUbo {
    float invViewProj[16];
    float yawRad;
    float pad[3];
};

struct VertexInput {
    std::array<float, 3> pos;
    std::array<float, 2> uv;
};

constexpr std::array vertex_input = {
    VertexInput { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } },
    VertexInput { { -1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
    VertexInput { { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } },
    VertexInput { { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
};

// Copy only the plain input fields.  The Vulkan handle members
// (vertex_buf/ubo_buf/pipeline) are move-only and empty at construction — they
// are built in prepare() — so the whole Desc isn't copyable.  Matches
// CustomShaderPass's ctor.
SkyboxPass::SkyboxPass(const Desc& desc) {
    m_desc.output       = desc.output;
    m_desc.pano_tex_key = desc.pano_tex_key;
}
SkyboxPass::~SkyboxPass() {}

namespace
{
// ToTexKey(SceneRenderTarget) is provided by PassCommon.hpp — reuse it.

// LOAD the existing _rt_default (PrePass already cleared it) and leave it in
// SHADER_READ_ONLY so the scene's own layer passes composite on top.  Same
// contract CustomShaderPass uses for a scene render-target output.
std::optional<vvk::RenderPass> CreateRenderPass(const vvk::Device& device, VkFormat format) {
    VkAttachmentDescription attachment {
        .format         = format,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkAttachmentReference attachment_ref {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &attachment_ref,
    };
    VkRenderPassCreateInfo creatinfo {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &attachment,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
    };
    vvk::RenderPass pass;
    if (auto res = device.CreateRenderPass(creatinfo, pass); res == VK_SUCCESS) {
        return pass;
    } else {
        VVK_CHECK(res);
        return std::nullopt;
    }
}
} // namespace

void SkyboxPass::prepare(Scene& scene, const Device& device, RenderingResources& rr) {
    WEK_PROFILE_SCOPE("SkyboxPass::prepare");
    m_fb_cache.clear();

    // Resolve the _rt_default output target.
    VkFormat output_format;
    {
        auto* rtp = scene.tryGetRenderTarget(m_desc.output);
        if (rtp == nullptr) {
            LOG_ERROR("skybox output RT '%s' missing", m_desc.output.c_str());
            return;
        }
        auto& rt      = *rtp;
        output_format = ToVkType(rt.format);
        if (auto opt = device.tex_cache().Query(m_desc.output, ToTexKey(rt), ! rt.allowReuse);
            opt.has_value()) {
            m_desc.vk_output = opt.value();
        } else {
            LOG_ERROR("skybox output RT query failed for '%s'", m_desc.output.c_str());
            return;
        }
    }

    // Resolve the panorama: decode the 2D image by key and upload it, the same
    // imported-texture path CustomShaderPass uses for a material texture.
    {
        if (m_desc.pano_tex_key.empty()) {
            LOG_ERROR("skybox pass has no panorama tex key");
            return;
        }
        if (! scene.imageParser) {
            LOG_ERROR("skybox: no image parser on scene");
            return;
        }
        auto image = scene.imageParser->Parse(m_desc.pano_tex_key);
        if (! image) {
            LOG_ERROR("skybox panorama '%s' failed to parse", m_desc.pano_tex_key.c_str());
            return;
        }
        auto slots = device.tex_cache().CreateTex(*image);
        if (slots.slots.empty()) {
            LOG_ERROR("skybox panorama '%s' upload failed", m_desc.pano_tex_key.c_str());
            return;
        }
        m_desc.vk_pano = slots.slots.front();
    }

    std::vector<Uni_ShaderSpv> spvs;
    {
        ShaderCompOpt opt;
        opt.client_ver             = glslang::EShTargetVulkan_1_1;
        opt.relaxed_errors_glsl    = true;
        opt.relaxed_rules_vulkan   = true;
        opt.suppress_warnings_glsl = true;

        std::array<ShaderCompUnit, 2> units;
        units[0] = ShaderCompUnit { .stage = EShLangVertex, .src = std::string(vert_code) };
        units[1] = ShaderCompUnit { .stage = EShLangFragment, .src = std::string(frag_code) };
        CompileAndLinkShaderUnits(units, opt, spvs);
    }

    VkVertexInputBindingDescription                bind_description;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
    {
        bind_description.stride    = sizeof(VertexInput);
        bind_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bind_description.binding   = 0;
        VkVertexInputAttributeDescription attr_pos {}, attr_uv {};
        attr_pos.binding  = 0;
        attr_pos.location = 0;
        attr_pos.format   = VK_FORMAT_R32G32B32_SFLOAT;
        attr_pos.offset   = offsetof(VertexInput, pos);
        attr_uv.binding   = 0;
        attr_uv.location  = 1;
        attr_uv.format    = VK_FORMAT_R32G32_SFLOAT;
        attr_uv.offset    = offsetof(VertexInput, uv);
        attr_descriptions.push_back(attr_pos);
        attr_descriptions.push_back(attr_uv);

        auto& buf = m_desc.vertex_buf;
        if (! rr.vertex_buf->allocateSubRef(sizeof(decltype(vertex_input)), buf)) return;
        rr.vertex_buf->writeToBuf(buf, { (uint8_t*)vertex_input.data(), buf.size });
    }

    // Two bindings: UBO (0) + panorama sampler (1), both push-descriptor.
    DescriptorSetInfo descriptor_info;
    {
        descriptor_info.push_descriptor = true;
        descriptor_info.bindings.resize(2);

        auto& ubo_binding           = descriptor_info.bindings[0];
        ubo_binding.binding         = 0;
        ubo_binding.descriptorCount = 1;
        ubo_binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo_binding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

        auto& pano_binding           = descriptor_info.bindings[1];
        pano_binding.binding         = 1;
        pano_binding.descriptorCount = 1;
        pano_binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pano_binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    {
        auto opt = CreateRenderPass(device.handle(), output_format);
        if (! opt.has_value()) return;
        auto pass = std::move(opt.value());

        GraphicsPipeline pipeline;
        pipeline.toDefault(); // depth test + write default OFF — skybox is the background
        pipeline.addDescriptorSetInfo(spanone { descriptor_info })
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
            .addInputBindingDescription(spanone { bind_description })
            .addInputAttributeDescription(attr_descriptions);
        for (auto& spv : spvs) pipeline.addStage(std::move(spv));

        if (! pipeline.create(device, pass, m_desc.pipeline)) return;
    }

    // Increment A: identity invViewProj (static background), yaw 0.  A later
    // increment writes the live camera + pan here each frame.
    {
        SkyboxUbo ubo {};
        for (int i = 0; i < 16; i++) ubo.invViewProj[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        ubo.yawRad = 0.0f;
        if (! rr.dyn_buf->allocateSubRef(
                sizeof(SkyboxUbo), m_desc.ubo_buf, device.limits().minUniformBufferOffsetAlignment))
            return;
        rr.dyn_buf->writeToBuf(m_desc.ubo_buf, { (uint8_t*)&ubo, sizeof(SkyboxUbo) });
    }

    setPrepared();
}

void SkyboxPass::execute(const Device& device, RenderingResources& rr) {
    WEK_PROFILE_SCOPE("SkyboxPass::execute");
    auto& cmd    = rr.command;
    auto& outext = m_desc.vk_output.extent;

    vvk::Framebuffer& framebuffer = m_fb_cache.getOrCreate(m_desc.vk_output.mip0_view, [&] {
        VkFramebufferCreateInfo info {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext           = nullptr,
            .renderPass      = *m_desc.pipeline.pass,
            .attachmentCount = 1,
            .pAttachments    = &m_desc.vk_output.mip0_view,
            .width           = m_desc.vk_output.extent.width,
            .height          = m_desc.vk_output.extent.height,
            .layers          = 1,
        };
        vvk::Framebuffer fb;
        (void)device.handle().CreateFramebuffer(info, fb);
        return fb;
    });

    // Push both descriptors in one call: panorama sampler (1) + UBO (0).
    {
        VkDescriptorImageInfo pano_img {
            .sampler     = m_desc.vk_pano.sampler,
            .imageView   = m_desc.vk_pano.view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkDescriptorBufferInfo ubo_info {
            rr.dyn_buf->gpuBuf(),
            m_desc.ubo_buf.offset,
            m_desc.ubo_buf.size,
        };
        std::array<VkWriteDescriptorSet, 2> wsets {
            VkWriteDescriptorSet {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext           = nullptr,
                .dstSet          = {},
                .dstBinding      = 1,
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo      = &pano_img,
            },
            VkWriteDescriptorSet {
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext           = nullptr,
                .dstSet          = {},
                .dstBinding      = 0,
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo     = &ubo_info,
            },
        };
        cmd.PushDescriptorSetKHR(
            VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wsets);
    }

    VkRenderPassBeginInfo pass_begin_info {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext       = nullptr,
        .renderPass  = *m_desc.pipeline.pass,
        .framebuffer = *framebuffer,
        .renderArea =
            VkRect2D {
                .offset = { 0, 0 },
                .extent = { outext.width, outext.height },
            },
        .clearValueCount = 0,
        .pClearValues    = nullptr,
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

    cmd.BindVertexBuffers(
        0, 1, std::array { rr.vertex_buf->gpuBuf() }.data(), &m_desc.vertex_buf.offset);
    cmd.Draw(4, 1, 0, 0);
    cmd.EndRenderPass();
}

void SkyboxPass::destory(const Device&, RenderingResources& rr) {
    setPrepared(false);
    clearReleaseTexs();
    m_fb_cache.clear();
    if (m_desc.vertex_buf) rr.vertex_buf->unallocateSubRef(m_desc.vertex_buf);
    if (m_desc.ubo_buf) rr.dyn_buf->unallocateSubRef(m_desc.ubo_buf);
}
