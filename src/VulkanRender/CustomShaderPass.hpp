#pragma once
#include "VulkanPass.hpp"
#include <string>
#include <vector>

#include "Vulkan/Device.hpp"
#include "Scene/Scene.h"
#include "Vulkan/StagingBuffer.hpp"
#include "Vulkan/GraphicsPipeline.hpp"
#include "SpriteAnimation.hpp"
#include "Interface/IShaderValueUpdater.h"

namespace wallpaper
{

namespace vulkan
{

class CustomShaderPass : public VulkanPass {
public:
    struct Desc {
        // in
        SceneNode*               node { nullptr };
        std::vector<std::string> textures;
        std::string              output;
        sprite_map_t             sprites_map;
        std::string              camera_override; // use named camera instead of node's

        // Reflection rendering overrides
        bool disableDepth { false };       // skip depth test/write
        bool flipCullMode { false };       // swap front/back cull (reflection reverses winding)
        bool useReflectionDepth { false }; // use separate depth buffer for reflection passes

        // MSAA
        VkSampleCountFlagBits msaaSamples { VK_SAMPLE_COUNT_1_BIT };
        VkImageView           msaaColorView { VK_NULL_HANDLE };
        VkImage               msaaColorImage { VK_NULL_HANDLE };

        // Geometry shader: use POINT_LIST topology instead of TRIANGLE_STRIP
        bool point_topology { false };

        // -----prepared
        // vulkan texs
        std::vector<ImageSlotsRef> vk_textures;
        std::vector<i32>           vk_tex_binding;
        ImageParameters            vk_output;
        ImageParameters            vk_fallback_tex; // 1x1 dummy for unbound slots

        // bufs
        bool                          dyn_vertex { false };
        std::vector<StagingBufferRef> vertex_bufs;
        StagingBufferRef              index_buf;
        StagingBufferRef              ubo_buf;

        // pipeline
        VkClearValue          clear_value;
        bool                  blending { false };
        bool                  hasDepth { false };
        VkImageView           depthView { VK_NULL_HANDLE };
        VkImage               depthImage { VK_NULL_HANDLE };
        std::shared_ptr<void> depthOwner;     // prevent shared depth image from being freed
        std::shared_ptr<void> msaaColorOwner; // prevent shared MSAA image from being freed
        vvk::Framebuffer      fb;
        PipelineParameters    pipeline;
        u32                   draw_count { 0 };

        // uniforms
        std::function<void()> update_op;
    };

    CustomShaderPass(const Desc&);
    virtual ~CustomShaderPass();

    void setDescTex(u32 index, std::string_view tex_key);

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

    // Returns true if this pass has no dynamic elements (vertices, sprites)
    bool isStatic() const override { return ! m_desc.dyn_vertex && m_desc.sprites_map.empty(); }

    // Returns true if shader uses time-based uniforms (g_Time, g_PointerPosition, etc.)
    bool usesTimeUniforms() const { return m_uses_time_uniforms; }

    // Pass is cacheable if static and doesn't use time-based uniforms
    bool isCacheable() const { return isStatic() && ! m_uses_time_uniforms; }

    // Cache is only safe to skip-on-reexecute when the pass's output image
    // is not aliased by a later pass in the frame order — otherwise the RT
    // contents are overwritten before the next frame and downstream passes
    // would read stale data.  Set by VulkanRender::compileRenderGraph after
    // all passes are prepared and their output VkImages are resolved.
    bool canCache() const { return m_can_cache; }
    void setCanCache(bool v) { m_can_cache = v; }

    // Check if output is already cached and valid
    bool isCached() const { return m_cached; }
    void invalidateCache() { m_cached = false; }
    void markCached() { m_cached = true; }

    // Accessors for per-pass debug dump (VulkanRender::Impl reads output image
    // info after a frame WaitIdle to produce PPMs of each pass's render target).
    const Desc& desc() const { return m_desc; }

private:
    Desc m_desc;
    bool m_cached { false };
    bool m_can_cache { false };
    bool m_uses_time_uniforms { false };
};

} // namespace vulkan
} // namespace wallpaper
