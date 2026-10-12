#pragma once
#include "VulkanPass.hpp"
#include "VulkanRender/PassCacheAlias.hpp"
#include "VulkanRender/PassMerge.hpp"
#include "VulkanRender/RtResidency.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "Vulkan/Device.hpp"
#include "Vulkan/Parameters.hpp"
#include "Vulkan/TextureCache.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneImageEffectLayer.h"
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

// Which kind of runtime-hideable chain pass this is. Terminal writes the
// layer's output and picks its passthrough source each frame; Middle writes a
// chain buffer and copies its one input when HiddenEffectMustCopy says so.
enum class HideableKind
{
    None,
    Terminal,
    Middle,
};

struct HideableDecision {
    bool draw_passthrough { false }; // bind the passthrough pipeline
    bool keep_real_draw { false };   // passthrough missing: draw the real material
};

// Decided every frame, before the hidden-pass skip guard. `own_hidden` is the
// effect's own visibility flag; `ancestors_visible` is the layer's (a hidden
// layer copies nothing: Normal blend would stamp transparent black over the
// scene). `must_copy` is HiddenEffectMustCopy for a Middle pass and true for a
// Terminal. When neither field is set the pass takes the ordinary path.
// `passthrough_ready` is false when prepare() failed to build the passthrough
// pipeline or its UBO; the null pipeline must never be bound, so the pass
// keeps its real draw (dropping it would blank a terminal's whole layer).
inline HideableDecision DecideHideablePass(HideableKind kind, bool own_hidden,
                                           bool ancestors_visible, bool must_copy,
                                           bool passthrough_ready) {
    if (kind == HideableKind::None || ! own_hidden || ! ancestors_visible || ! must_copy) return {};
    return { passthrough_ready, ! passthrough_ready };
}

// Which of a terminal pass's two physical ping-pong buffers holds
// "the last visible effect's output ahead of it, or the layer's own image
// when nothing ahead is visible" -- the thing the passthrough pipeline
// samples once DecideHideablePass says to draw it.
//
// SceneImageEffectLayer::ResolveEffect's swap_pp alternates each effect's
// write target starting at buffer B for effect 0, so a predecessor's output
// buffer is decided purely by its position's parity, and the un-effected
// base draw always lands in buffer A (the same physical resource effect 0
// reads as its input). Walking backward from the terminal's immediate
// predecessor to the first one that's actually visible this frame, and
// answering BufferA when none are, reproduces that rule exactly.
//
// A single fixed slot resolved once at graph-compile time gets this wrong
// the moment the immediate predecessor is ALSO hidden: that predecessor is
// an ordinary (non-terminal) pass, so it already takes the existing
// invisible-fast-path skip when hidden, and its buffer keeps holding
// whatever it drew the last time it was visible -- not this frame's answer.
// `predecessorVisible[i]` is effect i's own `visible` flag (see
// ChainVisibility), without the layer's visibility, for i in
// [0, terminalIndex).
enum class PassthroughSource
{
    BufferA,
    BufferB,
};

// The chain's per-effect visibility this frame, from each effect's own flag
// (set together with its nodes by the runtime drain). The layer's own
// visibility is deliberately not mixed in; DecideHideablePass handles that.
inline void ChainVisibilityInto(const std::vector<const SceneImageEffect*>& chain,
                                std::vector<bool>&                          v) {
    v.assign(chain.size(), false);
    for (usize i = 0; i < chain.size(); i++) v[i] = chain[i] != nullptr && chain[i]->visible;
}
inline std::vector<bool> ChainVisibility(const std::vector<const SceneImageEffect*>& chain) {
    std::vector<bool> v;
    ChainVisibilityInto(chain, v);
    return v;
}

inline PassthroughSource SelectPassthroughSource(const std::vector<bool>& predecessorVisible) {
    for (usize i = predecessorVisible.size(); i-- > 0;) {
        if (predecessorVisible[i]) {
            return (i % 2 == 0) ? PassthroughSource::BufferB : PassthroughSource::BufferA;
        }
    }
    return PassthroughSource::BufferA;
}

// Index of the nearest visible effect before `i`, or -1 when none is: -1
// stands for the layer's own image, which the base draw puts in buffer A.
inline i32 NearestVisibleBefore(usize i, const std::vector<bool>& effectVisible) {
    for (usize j = i; j-- > 0;)
        if (effectVisible[j]) return (i32)j;
    return -1;
}

// Must hidden effect `i` draw a passthrough copy this frame instead of being
// skipped?  Effect i reads buf(i-1) and writes buf(i), alternating between two
// buffers, so a visible effect j after a hidden run reads the output of the
// nearest visible effect k only when the run's length j-1-k is even. The last
// hidden effect before j fixes an odd run with one copy: it reads buf(i-1),
// which still holds k's output (an even distance, untouched by the skipped
// effects between), and writes the other buffer, so it never reads its own
// output. Every buffer any pass reads is then written this frame.
//
// The last effect is never asked: it writes the layer's output, not a chain
// buffer, and picks its source itself (SelectPassthroughSource).
// `baseImageWritten` is false for compose layers that do not draw their own
// image (copybackground off); copying that never-written buffer would only
// spread undefined contents.
inline bool HiddenEffectMustCopy(usize i, const std::vector<bool>& effectVisible,
                                 bool baseImageWritten) {
    if (effectVisible[i]) return false;
    if (i + 1 >= effectVisible.size()) return false;
    if (! effectVisible[i + 1]) return false;
    const i32 k = NearestVisibleBefore(i, effectVisible);
    if (k < 0 && ! baseImageWritten) return false;
    return ((i32)i - k) % 2 == 1;
}

// Everything execute() decides from visibility, decided once.  The per-frame
// demand walk reads the same struct, so what it keeps resident is exactly what
// the record will sample.
struct HideableFrame {
    bool             node_visible { true };      // node == nullptr || node->IsVisible()
    bool             own_hidden { false };       // the effect's own flag when the chain is known
    bool             ancestors_visible { true }; // node == nullptr || node->AncestorsVisible()
    bool             must_copy { false };
    HideableDecision hide;
    bool             records { true }; // false: execute returns before recording anything
    bool             draws { false };  // a draw call is issued (real material or passthrough)
    DemandInputs     inputs { DemandInputs::None };
    i32              passthrough_tex_index { -1 };
};

inline HideableFrame DecideHideable(HideableKind kind, const std::vector<bool>& chain_visible,
                                    i32 chain_index, bool has_node, bool node_visible,
                                    bool ancestors_visible, bool base_image_written,
                                    bool passthrough_ready, bool clears_output, PassMergeRole role,
                                    i32 pt_a, i32 pt_b) {
    HideableFrame f;
    f.node_visible      = ! has_node || node_visible;
    f.ancestors_visible = ! has_node || ancestors_visible;
    const bool chain_known =
        kind != HideableKind::None && SignedIndexInRange(chain_index, chain_visible.size());
    f.own_hidden = has_node && ! node_visible;
    if (chain_known) f.own_hidden = ! chain_visible[(usize)chain_index];
    f.must_copy = kind != HideableKind::Middle ||
                  (chain_known &&
                   HiddenEffectMustCopy((usize)chain_index, chain_visible, base_image_written));
    f.hide =
        DecideHideablePass(kind, f.own_hidden, f.ancestors_visible, f.must_copy, passthrough_ready);
    const bool records_while_hidden = f.hide.draw_passthrough || f.hide.keep_real_draw;
    if (has_node && ! records_while_hidden &&
        IsHiddenPassSkippable(! node_visible, clears_output) && HiddenPassDropsRecording(role)) {
        f.records = false;
        return f;
    }
    f.records = true;
    f.draws   = f.node_visible || f.hide.draw_passthrough || f.hide.keep_real_draw;
    if (! f.draws) return f; // a hidden clear owner or group boundary: records, samples nothing
    if (f.hide.draw_passthrough && kind == HideableKind::Middle) {
        f.inputs                = DemandInputs::PassthroughA;
        f.passthrough_tex_index = pt_a;
    } else if (f.hide.draw_passthrough && kind == HideableKind::Terminal) {
        const std::vector<bool> before =
            chain_known
                ? std::vector<bool>(chain_visible.begin(), chain_visible.begin() + chain_index)
                : std::vector<bool>();
        const bool from_b       = SelectPassthroughSource(before) == PassthroughSource::BufferB;
        f.inputs                = from_b ? DemandInputs::PassthroughB : DemandInputs::PassthroughA;
        f.passthrough_tex_index = from_b ? pt_b : pt_a;
    } else {
        f.inputs = DemandInputs::Own;
    }
    return f;
}

// Cached bytes are only right while every image they were drawn from is
// untouched, so a cached pass keeps its own inputs resident even on a frame
// it records nothing.
inline PassDemandFrame DemandFrameFor(const HideableFrame& hf, bool cached) {
    PassDemandFrame f { hf.draws, cached, hf.inputs };
    if (f.cached && f.inputs == DemandInputs::None) f.inputs = DemandInputs::Own;
    return f;
}

// A terminal pass appends 2 extra texture reads (the chain's ping-
// pong buffers), and a middle pass 1, on top of however many the real
// material's own JSON declares, so a texture-slot index can run past WE_GLTEX_NAMES' fixed 13
// entries (Wallpaper Engine materials only ever declare g_Texture0 through
// g_Texture12). Bounds-checked stand-in for indexing the array directly --
// returns null past the table instead of reading out of bounds.
inline const char* GLTexNameForIndex(usize index) {
    return index < WE_GLTEX_NAMES.size() ? WE_GLTEX_NAMES[index] : nullptr;
}

// True for a texture slot that only the passthrough pipeline reads: the slots
// a hideable pass appends after the material's own textures. The real
// pipeline never binds them, even when its shader happens to declare a
// g_Texture<N> with that index -- exactly as before the pass became hideable.
inline bool IsAppendedPassthroughSlot(usize index, i32 passthrough_a, i32 passthrough_b) {
    i32 first = -1;
    for (i32 v : { passthrough_a, passthrough_b })
        if (v >= 0 && (first < 0 || v < first)) first = v;
    return first >= 0 && index >= (usize)first;
}

// The descriptor binding of the material's own texture `index`, or -1 when the
// shader never declared it.  The passthrough's appended reads are not the
// material's textures; leaving them unbound keeps the real pipeline's
// descriptors as they were before a pass became hideable.
inline i32 MaterialTexBinding(usize index, i32 passthrough_a, i32 passthrough_b,
                              const Map<std::string, VkDescriptorSetLayoutBinding>& binding_map) {
    const char* tex_name = GLTexNameForIndex(index);
    if (IsAppendedPassthroughSlot(index, passthrough_a, passthrough_b)) tex_name = nullptr;
    if (tex_name != nullptr && exists(binding_map, tex_name))
        return (i32)binding_map.find(tex_name)->second.binding;
    return -1;
}

// One vertex-input attribute per (vertex array, shader input).  An input the
// array does not carry gets offset 0, as the shader still needs a location.
inline void AppendVertexArrayAttributes(
    std::vector<VkVertexInputAttributeDescription>& out, u32 binding,
    const Map<std::string, SceneVertexArray::SceneVertexAttributeOffset>& mesh_attrs,
    const Map<std::string, ShaderReflected::Input>&                       inputs) {
    for (auto& item : inputs) {
        const auto found = mesh_attrs.find(item.first);
        out.push_back(VkVertexInputAttributeDescription {
            .location = item.second.location,
            .binding  = binding,
            .format   = item.second.format,
            .offset   = (u32)(found != mesh_attrs.end() ? found->second.offset : 0),
        });
    }
}
inline std::vector<VkVertexInputAttributeDescription>
VertexAttributeDescriptions(const SceneMesh&                                mesh,
                            const Map<std::string, ShaderReflected::Input>& inputs) {
    std::vector<VkVertexInputAttributeDescription> out;
    for (uint i = 0; i < mesh.VertexCount(); i++)
        AppendVertexArrayAttributes(out, i, mesh.GetVertexArray(i).GetAttrOffsetMap(), inputs);
    return out;
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

// Rebuild the framebuffer when there is none and the output has an image, or
// when the image it was built over was replaced.  An absent output needs no
// framebuffer: the pass skips.
inline bool FramebufferNeedsRebuild(bool has_fb, std::uint32_t fb_generation, const RtSlot& out) {
    if (! out.resident()) return false;
    return ! has_fb || fb_generation != out.generation;
}

// How one material texture reaches the descriptor set on a drawing frame.
// Skip: the shader never declared it.  Image: the slot has an image.  Fallback:
// declared but absent, so the 1x1 dummy stands in.  Omit: declared and absent,
// but nothing to write (a record that does not draw, or no dummy exists).
enum class TexBind
{
    Skip,
    Image,
    Fallback,
    Omit,
};

// One entry per material texture: `index` into vk_textures / vk_tex_binding.
struct TexBindStep {
    TexBind how { TexBind::Skip };
    usize   index { 0 };
};

// Replacing a framebuffer a submitted frame may still be using: hand the old
// one to the retirer instead of letting the assignment destroy it.  False when
// there was nothing to retire or nobody to retire it to (the old one is then
// destroyed in place, which is only safe with the device idle).
template<typename Fb, typename Retirer>
bool RetireOldFramebuffer(Fb& old, const Retirer& retirer) {
    const bool has_old = *old != VK_NULL_HANDLE;
    if (! has_old || ! retirer) return false;
    retirer(std::move(old));
    return true;
}

// What ensureFramebuffer has to do before the pass can record: nothing (the
// framebuffer is usable, or this role never begins a render pass), give up
// for the frame, or build a new one.  A pass without a slot owns a fixed
// framebuffer and only reports whether it exists.
enum class FbStep
{
    Ready,
    Fail,
    Build,
};
template<typename Fb>
FbStep DecideFramebufferStep(bool opens, const RtSlot* out, const Fb& fb, std::uint32_t fb_gen) {
    if (! opens) return FbStep::Ready;
    const bool has_fb = *fb != VK_NULL_HANDLE;
    if (out == nullptr) return has_fb ? FbStep::Ready : FbStep::Fail;
    if (! out->resident()) return FbStep::Fail;
    return FramebufferNeedsRebuild(has_fb, fb_gen, *out) ? FbStep::Build : FbStep::Ready;
}

// Attachments match the render pass: [0]=color, [1]=depth; with MSAA
// [0]=msaa color, [1]=resolve, [2]=msaa depth.  Render attachments must be
// single-mip views; the caller passes the output's mip0 view.
struct FbAttachments {
    VkImageView   views[3] {};
    std::uint32_t count { 0 };
};
inline FbAttachments FramebufferAttachments(VkSampleCountFlagBits samples, bool has_depth,
                                            VkImageView msaa_color, VkImageView out,
                                            VkImageView depth) {
    FbAttachments a;
    if (samples > VK_SAMPLE_COUNT_1_BIT) {
        a.views[0] = msaa_color;
        a.views[1] = out;
        a.views[2] = depth;
        a.count    = has_depth ? 3u : 2u;
    } else {
        a.views[0] = out;
        a.views[1] = depth;
        a.count    = has_depth ? 2u : 1u;
    }
    return a;
}

// Only the pass that opens a merged group can abandon it: it is the one whose
// failed begin leaves the members with no render pass to record into.
inline bool FramebufferFailureAbandonsGroup(PassMergeRole role) {
    return role == PassMergeRole::GroupBegin;
}

// The slot at `i`, or null for a bitmap texture or an index past the list.
inline RtSlot* SlotAt(const std::vector<RtSlot*>& slots, usize i) {
    return i < slots.size() ? slots[i] : nullptr;
}

// Which texture the passthrough draw samples this frame: the index the chain
// picked, if the pass has a passthrough sampler and the index names a texture.
inline std::optional<usize> PassthroughSamplerIndex(i32 binding, i32 tex_index, usize n_textures) {
    if (binding >= 0 && tex_index >= 0 && (usize)tex_index < n_textures) return (usize)tex_index;
    return std::nullopt;
}

// A merged group's members record inside the render pass the GroupBegin
// opened.  If that begin never happened (its framebuffer could not be built),
// a member drawing anyway would record outside any render pass, so the
// members and the end follow the opener's failure and skip.
inline bool GroupMemberSkips(PassMergeRole role, bool group_abandoned) {
    return group_abandoned &&
           (role == PassMergeRole::GroupMiddle || role == PassMergeRole::GroupEnd);
}

// Log a framebuffer-creation failure once per streak: the first failure after
// a success reports, later ones stay quiet (the pass retries every frame), and
// a success re-arms the report.  `logged` records that this streak has reported.
inline bool ShouldLogFramebufferFailure(bool failed, bool& logged) {
    if (! failed) {
        logged = false;
        return false;
    }
    if (logged) return false;
    logged = true;
    return true;
}

class CustomShaderPass : public VulkanPass {
public:
    struct Desc {
        // in
        SceneNode*               node { nullptr };
        std::vector<std::string> textures;
        std::string              output;
        // The render target this pass writes and, per texture index, the one it
        // samples (nullptr for bitmap textures, the depth alias and unbound
        // slots).  Set in prepare(); the handle copies below are refreshed
        // from them by rebindImages().
        RtSlot*                  out_slot { nullptr };
        std::vector<RtSlot*>     in_slots;
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

        // Set by SceneToRenderGraph for the chain-writing pass of an effect that
        // carries a script or user-property visibility binding. Gates the
        // passthrough build in prepare() and the per-frame DecideHideablePass.
        HideableKind hideable { HideableKind::None };

        // Indices into `textures`/`vk_textures` of the extra reads a hideable
        // pass copies from. A terminal gets the chain's two physical ping-pong
        // buffers (SceneImageEffectLayer's FirstTarget/SecondTarget), both
        // registered as reads regardless of which single slot its own (real)
        // material happens to read. A middle pass gets only `_a`, the one
        // buffer it copies; `_b` stays -1. Both -1 when not hideable.
        // execute() picks between them every frame via SelectPassthroughSource
        // -- see that function for why a single fixed slot isn't enough.
        i32 passthrough_input_tex_index_a { -1 };
        i32 passthrough_input_tex_index_b { -1 };

        // The hideable pass's whole effect chain, in order, and its own index
        // in it. The effects outlive the graph: SceneImageEffectLayer only
        // removes effects (RemoveFailedEffects) before the graph is built.
        std::vector<const SceneImageEffect*> chain_effects {};
        i32                                  chain_index { -1 };

        // False for a compose layer that does not draw its own image into
        // buffer A (copybackground off); a hidden first effect then has
        // nothing valid to copy.
        bool chain_base_image_written { true };

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
        std::uint32_t                       fb_generation { 0 };
        PipelineParameters    pipeline;
        u32                   draw_count { 0 };

        // Whether this pass records its draw inside its own render pass
        // instance or shares one with the neighbours that write the same
        // attachments.  Assigned once per graph compile by
        // VulkanRender::compileRenderGraph; see PassMerge.hpp.
        PassMergeRole merge_role { PassMergeRole::Standalone };

        // uniforms
        std::function<void()> update_op;

        // Terminal-hideable passthrough (only built when `hideable` is set):
        // a second, minimal pipeline sharing this pass's render pass and
        // framebuffer, drawn instead of `pipeline` while the node is hidden.
        // See CustomShaderPass::prepare()'s hideable branch.
        PipelineParameters passthrough_pipeline;
        StagingBufferRef   passthrough_ubo_buf;
        // Descriptor binding number the passthrough fragment shader's sampler
        // reflects to (resolved from its own GenReflect pass; independent of
        // vk_tex_binding, which describes the REAL pipeline's bindings).
        i32                   passthrough_tex_binding { -1 };
        std::function<void()> passthrough_update_op;
        // Set by prepare() only once the passthrough pipeline, its UBO and
        // passthrough_update_op all exist.  Any failed step leaves it false,
        // and execute() then keeps the real draw (DecideHideablePass's keep_real_draw).
        bool passthrough_ready { false };
    };

    CustomShaderPass(const Desc&);
    virtual ~CustomShaderPass();

    void setDescTex(u32 index, std::string_view tex_key);

    // Re-copies the output and input handles from their slots when a slot's
    // generation moved or its image appeared or vanished.
    void rebindImages();

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
    // all passes are prepared and their output slots are resolved.
    bool canCache() const { return m_can_cache; }
    // The frame's one visibility verdict for this pass; execute() reads it once.
    HideableFrame frameHideable(PassMergeRole role) const;
    // The cache step at the top of execute(): whether the cached bytes are
    // dropped and whether the pass may return without recording.
    PassCacheDecision cacheDecision(PassMergeRole role, bool own_dirty, bool node_visible) const;
    // True when the output is a managed target with no image or nobody needing
    // it this frame; execute() then records nothing.
    bool outputSkipsRecord() const;
    // The per-frame half of this pass's demand, built from frameHideable().
    PassDemandFrame demandFrame() const override;
    void setCanCache(bool v) { m_can_cache = v; }
    // True for a frame in which a slot this pass samples has no image.  The
    // pass then takes the hidden path: nothing draws from a slot with no image.
    void setBlockedThisFrame(bool b) { m_blocked_frame = b; }

    // Check if output is already cached and valid
    bool isCached() const { return m_cached; }
    void invalidateCache() { m_cached = false; }
    // `visible` is the node's visibility at draw time: hiding or showing it
    // later does not dirty the material, so the cache has to notice itself.
    // `output_generation` is the output slot's image generation: a new image
    // means the cached bytes are gone.
    void markCached(bool visible, std::uint32_t output_generation) {
        m_cached                   = true;
        m_cached_visible           = visible;
        m_cached_output_generation = output_generation;
    }

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

    // Output keys drawn this frame and last frame, owned by
    // VulkanRender::Impl and wired in alongside the depth tracker.  A cached
    // pass consults them (see DecidePassCache) and every pass that really
    // records a draw adds its output to this frame's set.  Keyed on the
    // render target's slot, not its VkImage, which can change under a pass.
    void setRewrittenFrameSets(std::unordered_set<ResidencyKey>* this_frame,
                               std::unordered_set<ResidencyKey>* last_frame) {
        m_rewritten_frame_tracker      = this_frame;
        m_rewritten_last_frame_tracker = last_frame;
    }
    // The targets this pass samples, as compileRenderGraph resolved them for
    // the cache-safety analysis, and the subset written by a pass at or after
    // this one in execution order (read a frame late); the redraw check reads
    // both.
    void setInputKeys(std::vector<ResidencyKey> all, std::vector<ResidencyKey> backward) {
        m_input_keys          = std::move(all);
        m_backward_input_keys = std::move(backward);
    }
    const std::vector<ResidencyKey>& inputKeys() const { return m_input_keys; }
    const std::vector<ResidencyKey>& backwardInputKeys() const { return m_backward_input_keys; }
    // The key of the render target this pass draws into; null without a slot.
    // Defined after KeyForSlot, below the class.
    ResidencyKey outputKey() const;

    // Accessors for per-pass debug dump (VulkanRender::Impl reads output image
    // info after a frame WaitIdle to produce PPMs of each pass's render target).
    const Desc& desc() const { return m_desc; }

    // Moves the framebuffer out (for deferred destruction); empty if none was built.
    std::optional<vvk::Framebuffer> takeFramebuffer();

    // Where a framebuffer replaced by ensureFramebuffer goes, so it outlives
    // the frames still in flight.  Without one it is destroyed on the spot.
    using FramebufferRetirer = std::function<void(vvk::Framebuffer)>;
    void setFramebufferRetirer(FramebufferRetirer r) { m_fb_retirer = std::move(r); }

private:
    // Builds m_desc.fb over the output's current view when it is missing or was
    // built for another image.  False when there is nothing to draw into or
    // creation failed; the pass skips this frame and retries next.
    bool ensureFramebuffer(const Device& device, PassMergeRole role);

    // Clear this pass's depth image to 1.0 and leave it in
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL, recording the barriers around the
    // clear.  No-op once some pass has already done it for the same image
    // this frame.  Called from the normal record path and, so that a hidden
    // pass can hand the job over before it bails out, from the skip path.
    void recordDepthInit(const vvk::CommandBuffer& cmd);

    Desc                              m_desc;
    BoundImage                        m_out_bound;
    std::vector<BoundImage>           m_in_bound;
    std::unordered_set<VkImage>*      m_depth_inited_frame_tracker { nullptr };
    std::unordered_set<ResidencyKey>* m_rewritten_frame_tracker { nullptr };
    std::unordered_set<ResidencyKey>* m_rewritten_last_frame_tracker { nullptr };
    std::vector<ResidencyKey>         m_input_keys;
    std::vector<ResidencyKey>         m_backward_input_keys;
    bool                              m_cached { false };
    bool                              m_blocked_frame { false };
    bool                              m_cached_visible { true };
    std::uint32_t                     m_cached_output_generation { 0 };
    // Scratch for frameHideable(), which only the render looper calls (demandFrame and
    // execute), so no lock.
    mutable std::vector<bool>         m_chain_scratch;
    bool                              m_can_cache { false };
    bool                              m_uses_time_uniforms { false };
    bool                              m_samples_mutable_texture { false };
    bool                              m_fb_failure_logged { false };
    FramebufferRetirer                m_fb_retirer;

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
    std::vector<TexBindStep>           m_tex_bind_scratch;
};

// A slot's key is its id, so it survives the slot's image coming and going.
inline ResidencyKey KeyForSlot(const RtSlot* slot) {
    return slot == nullptr ? ResidencyKey {} : ResidencyKey::ForSlot(slot->id);
}

inline ResidencyKey CustomShaderPass::outputKey() const { return KeyForSlot(m_desc.out_slot); }

// The compile-time half of a pass's demand.  The two appended passthrough
// reads are the chain buffers a hidden effect copies through; they are only
// sampled on a frame DecideHideable picks one, so they are kept apart from
// the material's own textures.
inline PassDemandStatic PassDemandStaticFor(const CustomShaderPass::Desc& d) {
    PassDemandStatic s;
    s.out         = KeyForSlot(d.out_slot);
    s.out_managed = d.out_slot != nullptr && d.out_slot->managed();
    for (usize t = 0; t < d.in_slots.size(); ++t) {
        if (d.in_slots[t] == nullptr) continue;
        if (IsAppendedPassthroughSlot(
                t, d.passthrough_input_tex_index_a, d.passthrough_input_tex_index_b))
            continue;
        s.own_inputs.push_back(KeyForSlot(d.in_slots[t]));
    }
    auto slot_at = [&](i32 idx) {
        return SignedIndexInRange(idx, d.in_slots.size()) ? KeyForSlot(d.in_slots[(usize)idx])
                                                          : ResidencyKey {};
    };
    s.passthrough_a = slot_at(d.passthrough_input_tex_index_a);
    s.passthrough_b = slot_at(d.passthrough_input_tex_index_b);
    return s;
}

// A drawing pass whose managed input has no image would bind the opaque 1x1
// fallback in the buffer's place.  The demand walk makes every sampled slot
// resident before a pass may draw, so a hit here is a walk bug, not a
// condition to render through.
inline bool AbsentManagedInputBlocksDraw(const RtSlot* in, bool draws) {
    return draws && in != nullptr && in->managed() && ! in->resident();
}

// The same input on a record that does not draw: nothing samples it, so the
// bind loop skips the descriptor write instead of binding the fallback.
inline bool AbsentManagedInputSkipsBind(const RtSlot* in, bool draws) {
    return ! draws && in != nullptr && in->managed() && ! in->resident();
}

inline TexBind PlanTextureBind(i32 binding, bool has_image, const RtSlot* in, bool draws,
                               bool has_fallback) {
    if (binding < 0) return TexBind::Skip;
    if (has_image) return TexBind::Image;
    if (AbsentManagedInputSkipsBind(in, draws) || ! has_fallback) return TexBind::Omit;
    return TexBind::Fallback;
}
// Fills `plan` with one entry per texture.  A reused vector keeps its capacity,
// so a pass that records every frame allocates nothing in the steady state.
inline void PlanTextureBindsInto(std::vector<TexBindStep>&         plan,
                                 const std::vector<ImageSlotsRef>& textures,
                                 const std::vector<i32>&           bindings,
                                 const std::vector<RtSlot*>& in_slots, bool draws,
                                 bool has_fallback) {
    plan.clear();
    for (usize i = 0; i < textures.size(); i++)
        plan.push_back({ PlanTextureBind(bindings[i],
                                         ! textures[i].slots.empty(),
                                         SlotAt(in_slots, i),
                                         draws,
                                         has_fallback),
                         i });
}

} // namespace vulkan
} // namespace wallpaper
