#pragma once
#include "VulkanPass.hpp"
#include <string>
#include <vector>

#include "Vulkan/Device.hpp"
#include "Vulkan/Parameters.hpp"
#include "Scene/Scene.h"
#include "Vulkan/StagingBuffer.hpp"
#include "Vulkan/GraphicsPipeline.hpp"
#include "SpriteAnimation.hpp"
#include "Interface/IShaderValueUpdater.h"

namespace wallpaper
{

namespace vulkan
{

// Pure load-op selector for the per-pass output RT.  Returns CLEAR when:
//   - force_clear is set (per-light volumetric back-depth path), OR
//   - the RT has not yet been touched this frame (scene.clearedRTs check).
// Otherwise returns LOAD.  Pure on its two inputs — no Vulkan state.
inline VkAttachmentLoadOp SelectOutputLoadOp(bool force_clear, bool rt_already_cleared) {
    if (force_clear) return VK_ATTACHMENT_LOAD_OP_CLEAR;
    return rt_already_cleared ? VK_ATTACHMENT_LOAD_OP_LOAD
                              : VK_ATTACHMENT_LOAD_OP_CLEAR;
}

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

        // Volumetric / future depth-sampling chain.  When true, prepare()
        // wires _rt_sceneDepth as an additional sampled binding even if the
        // material JSON didn't name it in `textures` — used by passes whose
        // descriptor set is constructed programmatically (volumetric front).
        bool needsSceneDepth { false };

        // Re-clear the output RT at pass-start every frame regardless of
        // scene.clearedRTs state.  Used by the per-light volumetric back-depth
        // emission where multiple lights share _rt_volumetricsBack and the
        // one-shot clearedRTs gate would leak earlier lights' depth values
        // into later lights' integration math.
        bool force_clear_output { false };

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
        VkFormat                   vk_output_format { VK_FORMAT_UNDEFINED }; // for pass-dump
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
        // Typed (not shared_ptr<void>) so the barrier-emit site can read the
        // first-use latch (VmaImageParameters::initial_layout_transitioned)
        // without a static_cast.  The latch lives with the owning resource
        // so it dies when the VkImage dies — recycled handles can't fool a
        // stale tracker.  See barrier-emit site in CustomShaderPass.cpp.
        std::shared_ptr<VmaImageParameters> msaaColorOwner;
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

    // Returns true if any sampled texture is externally mutated out-of-band
    // (video frames uploaded via TextureCache::ReuploadTex reuse the same
    //  VkImage handle every frame — the render graph can't see the writes).
    bool samplesMutableTexture() const { return m_samples_mutable_texture; }

    // Pass is cacheable if static, doesn't use time-based uniforms, and
    // doesn't sample a texture that is re-uploaded out-of-band (video tex).
    bool isCacheable() const {
        return isStatic() && ! m_uses_time_uniforms && ! m_samples_mutable_texture;
    }

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
    bool m_samples_mutable_texture { false };

    // Per-pass scratch storage reused across execute() calls.
    //
    // Pre-reserved at end of prepare() to m_desc.vk_textures.size() (+1 for the
    // descriptor-writes vector to host the UBO writeset).  clear()ed at the top
    // of each execute() — std::vector::clear() preserves capacity per
    // [vector.modifiers]/p1, so subsequent push_backs hit no allocator while
    // size() <= capacity().
    //
    // Invariant (load-bearing): m_image_infos_scratch must NEVER reallocate
    // during execute(): m_descriptor_writes_scratch stores raw pImageInfo
    // pointers into it (&m_image_infos_scratch.back()).  Reserving capacity
    // in prepare() guarantees no realloc; the unit test pins this contract.
    // If a future maintainer changes the reserve or removes the prepare-time
    // bound, the test fails before shipping the dangling-pImageInfo bug.
    //
    // Storage choice: per-pass member.  Single-threaded record path,
    // prepare-time-bounded capacity, lifetime is local to the class.  A
    // thread_local alternative is a one-line swap if profiling later flags
    // per-pass capacity as resident-memory pressure.
    std::vector<VkImageMemoryBarrier>  m_image_barriers_scratch;
    std::vector<VkDescriptorImageInfo> m_image_infos_scratch;
    std::vector<VkWriteDescriptorSet>  m_descriptor_writes_scratch;
};

} // namespace vulkan
} // namespace wallpaper
