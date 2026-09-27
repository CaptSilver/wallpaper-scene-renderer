#pragma once
#include "VulkanPass.hpp"
#include "VulkanRender/PassMerge.hpp"
#include <string>
#include <unordered_set>
#include <vector>

#include "Vulkan/Device.hpp"
#include "Vulkan/Parameters.hpp"
#include "Scene/Scene.h"
#include "Vulkan/StagingBuffer.hpp"
#include "Vulkan/GraphicsPipeline.hpp"
#include "SpriteAnimation.hpp"
#include "Interface/IShaderValueUpdater.h"
#include "SpecTexs.hpp"
#include "Vulkan/Shader.hpp"
#include "WPMediaSystemTexture.hpp"

namespace wallpaper
{

namespace vulkan
{

// Index buffers arrive either packed two-per-slot (16-bit) or one-per-slot
// (32-bit).  The array carries the tag; this maps it to what the bind wants.
inline VkIndexType ToVkIndexType(SceneIndexArray::IndexWidth width) {
    return width == SceneIndexArray::IndexWidth::U32 ? VK_INDEX_TYPE_UINT32
                                                     : VK_INDEX_TYPE_UINT16;
}

// vkCmdBindIndexBuffer requires the buffer offset be a multiple of the index
// size, so the sub-allocation has to be aligned to it.
inline VkDeviceSize IndexAlignmentFor(VkIndexType type) {
    return type == VK_INDEX_TYPE_UINT32 ? 4u : 2u;
}

// Wallpaper Engine materials say "normal" or "nocull"; they never say
// "back"/"front", which are this engine's own vocabulary.  An unrecognised
// value used to fall through to no culling at all, so every model drew both
// hemispheres — wasted fill on opaque meshes, and doubled compositing on
// translucent shells such as a cloud or atmosphere layer.
inline VkCullModeFlags CullModeFor(std::string_view cullmode, bool flip) {
    const bool cull_back = (cullmode == "normal" || cullmode == "back");
    const bool cull_front = (cullmode == "front");
    if (! cull_back && ! cull_front) return VK_CULL_MODE_NONE;

    const bool back = cull_back != flip;
    return back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
}

// Pure load-op selector for the per-pass output RT.  Returns CLEAR when:
//   - force_clear is set (per-light volumetric back-depth path), OR
//   - the RT has not yet been touched this frame (scene.clearedRTs check).
// Otherwise returns LOAD.  Pure on its two inputs — no Vulkan state.
inline VkAttachmentLoadOp SelectOutputLoadOp(bool force_clear, bool rt_already_cleared) {
    if (force_clear) return VK_ATTACHMENT_LOAD_OP_CLEAR;
    return rt_already_cleared ? VK_ATTACHMENT_LOAD_OP_LOAD
                              : VK_ATTACHMENT_LOAD_OP_CLEAR;
}

// Can a pass whose node is hidden drop its whole execute() — no render pass
// begin/end, no barriers, no descriptor pushes, and on MSAA targets no
// full-screen resolve — instead of recording an empty pass that draws nothing?
//
// Hidden is not enough on its own.  `clears_output` says this pass is the one
// that initialises its render target: SelectOutputLoadOp hands CLEAR to the
// first writer of an RT and LOAD to every writer after it, baked in at
// prepare time.  Drop the pass that owns the clear and the image keeps
// whatever the previous frame left in it — nothing at all on the first frame —
// and the effect chain that samples it downstream composites that instead.
// The image also never reaches SHADER_READ_ONLY_OPTIMAL, which its reader
// expects.  A later writer is safe to drop precisely because an earlier,
// visible pass already produced and transitioned those contents; leaving them
// alone is exactly what a draw-less LOAD would have done.
inline bool IsHiddenPassSkippable(bool node_hidden, bool clears_output) {
    return node_hidden && ! clears_output;
}

// Path-A depth-image usage flags: SAMPLED_BIT is appended iff the device
// advertises sampled-image support for D32_SFLOAT on optimal tiling
// (Device::d32_sampleable()); the attachment/transfer-dst bits are
// unconditional.  A free function so the bit list has one definition rather
// than being spelled out at each place that needs to know it.
inline VkImageUsageFlags DepthImageUsageFlags(bool d32_sampleable) {
    return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
           (d32_sampleable ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u);
}

// Is a _rt_sceneDepth/_rt_volumetricsSingle texture request eligible for the
// path-A binding (sampling the live depth attachment directly) rather than
// being skipped this frame?  Mirrors CustomShaderPass::prepare()'s IsSpecTex
// branch condition: the alias key, a sampleable D32 device, no MSAA, and not
// inside a reflection pass (which owns a separate depth buffer).
inline bool DepthAliasPathAEligible(bool is_depth_alias, bool d32_sampleable, uint32_t msaaSamples,
                                    bool useReflectionDepth) {
    return is_depth_alias && d32_sampleable && msaaSamples <= 1 && ! useReflectionDepth;
}

// A pass declaring needsSceneDepth wires WE_SCENE_DEPTH into its texture
// list even when the material JSON never named it (volumetric-front and
// other passes that build their descriptor set programmatically).  Returns
// whether it appended the key — false means the list already named it, or
// the pass never asked for scene depth.
inline bool EnsureSceneDepthTextureIfNeeded(std::vector<std::string>& textures,
                                            bool                      needsSceneDepth) {
    if (! needsSceneDepth) return false;
    for (auto& t : textures) {
        if (t == WE_SCENE_DEPTH) return false;
    }
    textures.emplace_back(WE_SCENE_DEPTH);
    return true;
}

// Whether a UBO block declares any of the nine globals that change every
// frame (clock time, day/night cycle, mouse position, the three
// audio-spectrum band pairs).  A pass whose block needs none of these can
// cache its uniform state instead of re-uploading it every frame.
inline bool UsesTimeUniforms(const ShaderReflected::Block& block) {
    return exists(block.member_map, G_TIME) || exists(block.member_map, G_DAYTIME) ||
           exists(block.member_map, G_POINTERPOSITION) ||
           exists(block.member_map, G_AUDIOSPECTRUM16LEFT) ||
           exists(block.member_map, G_AUDIOSPECTRUM16RIGHT) ||
           exists(block.member_map, G_AUDIOSPECTRUM32LEFT) ||
           exists(block.member_map, G_AUDIOSPECTRUM32RIGHT) ||
           exists(block.member_map, G_AUDIOSPECTRUM64LEFT) ||
           exists(block.member_map, G_AUDIOSPECTRUM64RIGHT);
}

// Any shader-declared texture binding (bindings[i] >= 0) with no image slots
// wired needs the 1x1 dummy fallback so the descriptor set has something
// bound for every sampler the shader compiled against.  bindings[i] == -1
// means the shader never declared that texture at all -- not a gap.
inline bool NeedsFallbackTexture(const std::vector<i32>&           bindings,
                                 const std::vector<ImageSlotsRef>& textures) {
    for (usize i = 0; i < textures.size(); i++) {
        if (bindings[i] >= 0 && textures[i].slots.empty()) return true;
    }
    return false;
}

// Does this pass's texture list name a "Now Playing" system slot
// ($mediaThumbnail / $mediaPreviousThumbnail)? Those get reuploaded
// out-of-band by an MPRIS art event, the same way a rasterized text layer's
// _text_<id> key does -- prepare() ORs this into m_samples_mutable_texture so
// the pass never gets marked cacheable, which would freeze it on whatever it
// first sampled. Pulled out of the per-texture loop so the decision doesn't
// need a live Device to test.
inline bool ReferencesMediaSystemTexture(const std::vector<std::string>& textures) {
    for (auto& tex_name : textures) {
        if (IsKnownMediaSystemTexture(tex_name)) return true;
    }
    return false;
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
        VkIndexType                   index_type { VK_INDEX_TYPE_UINT16 };
        StagingBufferRef              ubo_buf;

        // pipeline.  clears_output mirrors the colour attachment's
        // prepare-time load op: true means CLEAR, i.e. this pass is the first
        // writer of its RT this frame and owns the target's initialisation.
        // Read by IsHiddenPassSkippable.
        VkClearValue          clear_value;
        bool                  clears_output { false };
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

        // Whether this pass records its draw inside its own render pass
        // instance or shares one with the neighbours that write the same
        // attachments.  Assigned once per graph compile by
        // VulkanRender::compileRenderGraph; see PassMerge.hpp.
        PassMergeRole merge_role { PassMergeRole::Standalone };

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

    // Assigned by VulkanRender::compileRenderGraph once every pass is
    // prepared and its attachment handles are known.
    void setMergeRole(PassMergeRole r) { m_desc.merge_role = r; }

    // Frame-scoped record of which depth images this frame's passes have
    // already cleared+transitioned (recordDepthInit's per-handle latch).
    // Owned by the VulkanRender::Impl that built this pass
    // (m_depth_inited_frame in VulkanRender.cpp) and wired in here by
    // compileRenderGraph every time it rebuilds m_passes -- not at
    // construction, since a single Impl rebuilds its passes many times over
    // a process's life while the tracker itself stays put.
    //
    // This used to be a namespace-scope global (g_depth_inited_frame) in
    // CustomShaderPass.cpp.  That let its destructor run via ordinary
    // process-exit static destruction on libc's own clock, with no relation
    // to whether the render thread was still mid-frame and reading it --
    // ASAN caught the resulting heap-use-after-free in drawFrameOffscreen,
    // twice, independently.  A member of the Impl, which ~MainHandler
    // already guarantees only dies after the render Looper is joined (see
    // SceneWallpaper.cpp), can't be torn down out from under a thread still
    // using it.
    void setDepthInitedFrameSet(std::unordered_set<VkImage>* tracker) {
        m_depth_inited_frame_tracker = tracker;
    }
    // Whether execute()/recordDepthInit() have anything wired to read/insert
    // into.  Null until compileRenderGraph wires it (or on a Desc a test
    // constructs directly and never runs through execute()).
    bool hasDepthInitedFrameSet() const { return m_depth_inited_frame_tracker != nullptr; }

    // Accessors for per-pass debug dump (VulkanRender::Impl reads output image
    // info after a frame WaitIdle to produce PPMs of each pass's render target).
    const Desc& desc() const { return m_desc; }

private:
    // Clear this pass's depth image to 1.0 and leave it in
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL, recording the barriers around the
    // clear.  No-op once some pass has already done it for the same image
    // this frame.  Called from the normal record path and, so that a hidden
    // pass can hand the job over before it bails out, from the skip path.
    void recordDepthInit(const vvk::CommandBuffer& cmd);

    Desc                         m_desc;
    std::unordered_set<VkImage>* m_depth_inited_frame_tracker { nullptr };
    bool                         m_cached { false };
    bool                         m_can_cache { false };
    bool                         m_uses_time_uniforms { false };
    bool                         m_samples_mutable_texture { false };

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
