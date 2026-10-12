#include <doctest.h>

#include "VulkanRender/CustomShaderPass.hpp"
#include "Vulkan/Parameters.hpp"
#include "Vulkan/Device.hpp"
#include "VulkanRender/CopyPass.hpp"

using wallpaper::vulkan::CustomShaderPass;
using wallpaper::vulkan::VmaImageParameters;

namespace
{
// Stands in for vvk::Framebuffer: what the decisions read is `*fb`.
struct FakeFb {
    VkFramebuffer h { VK_NULL_HANDLE };
    VkFramebuffer operator*() const { return h; }
};
VkFramebuffer fb(std::uintptr_t v) { return reinterpret_cast<VkFramebuffer>(v); }
} // namespace

// Pin the default-constructed Desc's capability flags.  needsSceneDepth must
// default to false so existing passes continue to behave exactly as before;
// only volumetric-front (and future SSAO/DOF) flips it on.
TEST_SUITE("CustomShaderPass::Desc") {
    TEST_CASE("needsSceneDepth defaults to false") {
        CustomShaderPass::Desc desc;
        CHECK(desc.needsSceneDepth == false);
    }
    TEST_CASE("disableDepth / flipCullMode / useReflectionDepth still default false") {
        // Regression guard — adding needsSceneDepth in the same struct must
        // not flip any other flag's default.
        CustomShaderPass::Desc desc;
        CHECK(desc.disableDepth == false);
        CHECK(desc.flipCullMode == false);
        CHECK(desc.useReflectionDepth == false);
    }

    // Index buffers used to be bound as UINT16 unconditionally, because every
    // producer packed two 16-bit indices per 32-bit slot.  Meshes with more
    // than 65535 vertices cannot do that, so the bind now follows the array.
    TEST_CASE("index_type defaults to UINT16") {
        CustomShaderPass::Desc desc;
        CHECK(desc.index_type == VK_INDEX_TYPE_UINT16);
    }

    TEST_CASE("index type follows the array width") {
        CHECK(wallpaper::vulkan::ToVkIndexType(wallpaper::SceneIndexArray::IndexWidth::U16) ==
              VK_INDEX_TYPE_UINT16);
        CHECK(wallpaper::vulkan::ToVkIndexType(wallpaper::SceneIndexArray::IndexWidth::U32) ==
              VK_INDEX_TYPE_UINT32);
    }
}

// MSAA first-use barrier latch lives on VmaImageParameters so it dies with
// the VkImage handle.  If the Vulkan driver recycles a freed VkImage handle
// for a newly created image, a global handle-keyed tracker would skip the
// UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL barrier on the recycled image's
// first use; the per-owner bool starts fresh every allocation.
TEST_SUITE("CustomShaderPass msaa init handle-reuse") {
    TEST_CASE("VmaImageParameters::initial_layout_transitioned defaults to false") {
        VmaImageParameters a;
        CHECK_FALSE(a.initial_layout_transitioned);
    }
    TEST_CASE("initial_layout_transitioned survives move-construction") {
        // Move ctor must propagate the latch (partial user-defined move ctors
        // silently drop new fields — see parseimageobj-decompose footgun).
        VmaImageParameters a;
        a.initial_layout_transitioned = true;
        VmaImageParameters b(std::move(a));
        CHECK(b.initial_layout_transitioned);
    }
    TEST_CASE("initial_layout_transitioned survives move-assignment") {
        VmaImageParameters a;
        a.initial_layout_transitioned = true;
        VmaImageParameters c;
        c = std::move(a);
        CHECK(c.initial_layout_transitioned);
    }
    TEST_CASE("Desc.msaaColorOwner stores a typed VmaImageParameters owner") {
        // The latch must be readable through the owner pointer without a
        // static_cast — a typed shared_ptr<VmaImageParameters>, not the
        // type-erased shared_ptr<void> the old global-tracker design used.
        CustomShaderPass::Desc desc;
        CHECK(desc.msaaColorOwner == nullptr);

        auto owner          = std::make_shared<VmaImageParameters>();
        desc.msaaColorOwner = owner;
        REQUIRE(desc.msaaColorOwner);
        CHECK_FALSE(desc.msaaColorOwner->initial_layout_transitioned);

        owner->initial_layout_transitioned = true;
        CHECK(desc.msaaColorOwner->initial_layout_transitioned);
    }
    TEST_CASE("Recycled VkImage handle on a fresh owner starts un-transitioned") {
        // Contract: when the driver reuses a freed VkImage handle for a new
        // resource, the per-image init flag MUST NOT incorrectly assume
        // "already inited" and skip the necessary UNDEFINED ->
        // ColorAttachmentOptimal layout transition.  Binding the flag to the
        // owning VmaImageParameters (instead of a global handle set) makes
        // the contract a class invariant: the bool dies with the owner, so
        // a recycled handle on a fresh owner always reports false.
        auto first                         = std::make_shared<VmaImageParameters>();
        first->initial_layout_transitioned = true; // simulate a first-use barrier emitted

        // first is destroyed (its VkImage handle is freed and may be recycled).
        first.reset();

        // A new owner is allocated; the driver could hand back the same
        // VkImage handle.  The latch on the new owner must still be false.
        auto recycled = std::make_shared<VmaImageParameters>();
        CHECK_FALSE(recycled->initial_layout_transitioned);
    }
}

// Cull mode comes from the material's own vocabulary.  Front faces are
// counter-clockwise after the projection's z-flip cancels the viewport's
// negative height, so "normal" means cull the back faces — verified on real
// sphere geometry, where back-culling leaves the near hemisphere visible and
// correctly oriented.
TEST_SUITE("CustomShaderPass cull mode") {
    using wallpaper::vulkan::CullModeFor;

    TEST_CASE("normal culls back faces") {
        CHECK(CullModeFor("normal", false) == VK_CULL_MODE_BACK_BIT);
    }

    TEST_CASE("nocull disables culling") {
        CHECK(CullModeFor("nocull", false) == VK_CULL_MODE_NONE);
    }

    TEST_CASE("an unknown cullmode disables culling rather than guessing") {
        CHECK(CullModeFor("", false) == VK_CULL_MODE_NONE);
        CHECK(CullModeFor("sideways", false) == VK_CULL_MODE_NONE);
    }

    TEST_CASE("back and front keep their explicit meaning") {
        CHECK(CullModeFor("back", false) == VK_CULL_MODE_BACK_BIT);
        CHECK(CullModeFor("front", false) == VK_CULL_MODE_FRONT_BIT);
    }

    // Reflection passes render the scene mirrored, which reverses winding.
    TEST_CASE("the flip swaps front and back but leaves nocull alone") {
        CHECK(CullModeFor("normal", true) == VK_CULL_MODE_FRONT_BIT);
        CHECK(CullModeFor("back", true) == VK_CULL_MODE_FRONT_BIT);
        CHECK(CullModeFor("front", true) == VK_CULL_MODE_BACK_BIT);
        CHECK(CullModeFor("nocull", true) == VK_CULL_MODE_NONE);
    }
}

TEST_SUITE("CustomShaderPass index alignment") {
    using wallpaper::vulkan::IndexAlignmentFor;

    // A default alignment of 1 happened to work only because every prior
    // sub-allocation was a multiple of 4.
    TEST_CASE("alignment matches the index size") {
        CHECK(IndexAlignmentFor(VK_INDEX_TYPE_UINT16) == 2u);
        CHECK(IndexAlignmentFor(VK_INDEX_TYPE_UINT32) == 4u);
    }
}

TEST_SUITE("CustomShaderPass time-uniform detection") {
    using wallpaper::vulkan::UsesTimeUniforms;
    using Block = wallpaper::vulkan::ShaderReflected::Block;

    TEST_CASE("a block naming only g_Time counts as time-based") {
        Block block;
        block.member_map[std::string(wallpaper::G_TIME)] = {};
        CHECK(UsesTimeUniforms(block));
    }
    TEST_CASE("a block naming only the 64-band right audio spectrum counts as time-based") {
        Block block;
        block.member_map[std::string(wallpaper::G_AUDIOSPECTRUM64RIGHT)] = {};
        CHECK(UsesTimeUniforms(block));
    }
    TEST_CASE("a block naming none of the nine globals is not time-based") {
        Block block;
        block.member_map["g_SomeOtherUniform"] = {};
        CHECK_FALSE(UsesTimeUniforms(block));
    }
    TEST_CASE("an empty block is not time-based") {
        Block block;
        CHECK_FALSE(UsesTimeUniforms(block));
    }
}

// One decision for every hideable pass, taken each frame before the
// hidden-pass skip guard: draw the passthrough copy, draw the real material
// as a fallback, or neither (the pass then takes the ordinary skip path).
TEST_SUITE("CustomShaderPass::DecideHideablePass") {
    using wallpaper::vulkan::DecideHideablePass;
    using wallpaper::vulkan::HideableKind;

    TEST_CASE("a pass with no binding never copies or falls back") {
        auto d = DecideHideablePass(HideableKind::None, true, true, true, true);
        CHECK_FALSE(d.draw_passthrough);
        CHECK_FALSE(d.keep_real_draw);
    }
    TEST_CASE("a visible hideable pass draws its real material") {
        auto d = DecideHideablePass(HideableKind::Terminal, false, true, true, true);
        CHECK_FALSE(d.draw_passthrough);
        CHECK_FALSE(d.keep_real_draw);
    }
    TEST_CASE("a hidden terminal draws the passthrough") {
        auto d = DecideHideablePass(HideableKind::Terminal, true, true, true, true);
        CHECK(d.draw_passthrough);
        CHECK_FALSE(d.keep_real_draw);
    }
    TEST_CASE("a hidden middle effect copies only when the chain rule says so") {
        CHECK(DecideHideablePass(HideableKind::Middle, true, true, true, true).draw_passthrough);
        auto skip = DecideHideablePass(HideableKind::Middle, true, true, false, true);
        CHECK_FALSE(skip.draw_passthrough);
        CHECK_FALSE(skip.keep_real_draw);
    }
    TEST_CASE("inside a hidden layer nothing copies, so nothing is stamped over the scene") {
        for (auto kind : { HideableKind::Terminal, HideableKind::Middle }) {
            auto d = DecideHideablePass(kind, true, /*ancestors_visible=*/false, true, true);
            CHECK_FALSE(d.draw_passthrough);
            CHECK_FALSE(d.keep_real_draw);
        }
    }
    TEST_CASE("with no passthrough pipeline a needed copy falls back to the real draw") {
        for (auto kind : { HideableKind::Terminal, HideableKind::Middle }) {
            auto d = DecideHideablePass(kind, true, true, true, /*passthrough_ready=*/false);
            CHECK_FALSE(d.draw_passthrough);
            CHECK(d.keep_real_draw);
        }
    }
}

// SelectPassthroughSource picks which of the chain's two physical ping-pong
// buffers the passthrough pipeline samples, given the visibility of every
// effect ahead of the terminal (index 0..N-1, N-1 being the terminal's
// immediate predecessor).  SceneImageEffectLayer::ResolveEffect's swap_pp
// alternates each effect's OUTPUT between buffer B (even index) and buffer A
// (odd index), and the un-effected base draw always lands in buffer A too --
// so walking backward from the last predecessor to the first visible one,
// and falling back to buffer A when none are, is exactly "the last visible
// effect's output, or the layer's own image when nothing ahead is visible".
//
// A fixed, compile-time-resolved slot (this predicate's predecessor) gets
// this wrong whenever the terminal's immediate predecessor is ALSO hidden:
// that predecessor is a non-terminal pass, so it already takes the ordinary
// skip path when hidden (CustomShaderPass's existing invisible-fast-path),
// leaving its buffer holding stale content from whenever it was last drawn
// instead of this frame's real answer.  Object 174's "Album Cover" layer
// hits exactly this: Current (index 1) and Previous (the terminal) hide
// together on an artless track, and the fixed slot would show Current's
// stale last-drawn thumbnail instead of Defualt's (index 0) fresh output.
TEST_SUITE("CustomShaderPass::SelectPassthroughSource") {
    using wallpaper::vulkan::PassthroughSource;
    using wallpaper::vulkan::SelectPassthroughSource;

    TEST_CASE("no predecessors (single-effect chain, terminal IS effect 0) -> buffer A, the "
              "layer's own image") {
        CHECK(SelectPassthroughSource({}) == PassthroughSource::BufferA);
    }

    TEST_CASE("one predecessor") {
        // visible -> its own output, buffer B (index 0 is even)
        CHECK(SelectPassthroughSource({ true }) == PassthroughSource::BufferB);
        // hidden, nothing before it -> the layer's own image, buffer A
        CHECK(SelectPassthroughSource({ false }) == PassthroughSource::BufferA);
    }

    TEST_CASE("two predecessors (Album Cover's Defualt/Current shape)") {
        // both hidden -> buffer A
        CHECK(SelectPassthroughSource({ false, false }) == PassthroughSource::BufferA);
        // only Current (index 1, odd) visible -> buffer A, its own output
        CHECK(SelectPassthroughSource({ false, true }) == PassthroughSource::BufferA);
        // only Defualt (index 0, even) visible, Current hidden -> buffer B --
        // the exact failure scenario: a fixed slot pointed at Current's own
        // (stale) buffer A instead.
        CHECK(SelectPassthroughSource({ true, false }) == PassthroughSource::BufferB);
        // both visible -> Current (last, index 1) wins -> buffer A
        CHECK(SelectPassthroughSource({ true, true }) == PassthroughSource::BufferA);
    }

    TEST_CASE("three predecessors -- every hidden-run pattern") {
        CHECK(SelectPassthroughSource({ false, false, false }) == PassthroughSource::BufferA);
        CHECK(SelectPassthroughSource({ false, false, true }) == PassthroughSource::BufferB);
        CHECK(SelectPassthroughSource({ false, true, false }) == PassthroughSource::BufferA);
        CHECK(SelectPassthroughSource({ false, true, true }) == PassthroughSource::BufferB);
        CHECK(SelectPassthroughSource({ true, false, false }) == PassthroughSource::BufferB);
        CHECK(SelectPassthroughSource({ true, false, true }) == PassthroughSource::BufferB);
        CHECK(SelectPassthroughSource({ true, true, false }) == PassthroughSource::BufferA);
        CHECK(SelectPassthroughSource({ true, true, true }) == PassthroughSource::BufferB);
    }
}

// Pins the new Desc fields' defaults so an untouched pass (no
// SceneToRenderGraph wiring) behaves exactly like today: no passthrough
// anything gets built or bound.
TEST_SUITE("CustomShaderPass::Desc terminal-hideable fields") {
    TEST_CASE("hideable defaults to None") {
        CustomShaderPass::Desc desc;
        CHECK(desc.hideable == wallpaper::vulkan::HideableKind::None);
    }
    TEST_CASE("passthrough_input_tex_index_a and _b default to -1") {
        CustomShaderPass::Desc desc;
        CHECK(desc.passthrough_input_tex_index_a == -1);
        CHECK(desc.passthrough_input_tex_index_b == -1);
    }
    TEST_CASE("chain_effects defaults empty and chain_index to -1") {
        CustomShaderPass::Desc desc;
        CHECK(desc.chain_effects.empty());
        CHECK(desc.chain_index == -1);
    }
    TEST_CASE("passthrough_tex_binding defaults to -1") {
        CustomShaderPass::Desc desc;
        CHECK(desc.passthrough_tex_binding == -1);
    }
}

// IsHiddenPassSkippable alone would let a hidden hideable pass on a
// non-clearing RT (the shared _rt_default, whenever this object isn't the
// first thing drawn onto the composite) take the early-return skip and never
// record anything -- silently breaking the "hiding it still draws something"
// contract the passthrough pipeline depends on.  The guard at
// CustomShaderPass::execute()'s invisible-node fast path is exempted by
// DecideHideablePass; this wrapper derives the exemption from it the same
// way, so it is covered without a live Device.
namespace
{
bool wouldTakeHiddenSkipPath(wallpaper::vulkan::HideableKind kind, bool own_hidden,
                             bool ancestors_visible, bool passthrough_ready, bool clears_output,
                             bool must_copy = true) {
    const auto hide = wallpaper::vulkan::DecideHideablePass(
        kind, own_hidden, ancestors_visible, must_copy, passthrough_ready);
    const bool records_while_hidden = hide.draw_passthrough || hide.keep_real_draw;
    return ! records_while_hidden && wallpaper::vulkan::IsHiddenPassSkippable(
                                         own_hidden || ! ancestors_visible, clears_output);
}
} // namespace

TEST_SUITE("CustomShaderPass hideable-pass skip-guard exemption") {
    using wallpaper::vulkan::HideableKind;

    TEST_CASE("an ordinary hidden pass on a non-clearing RT takes the skip path") {
        CHECK(wouldTakeHiddenSkipPath(HideableKind::None, true, true, true, false));
    }
    TEST_CASE("a hidden hideable pass on the SAME shape does NOT take the skip path") {
        // clears_output=false is exactly the shared _rt_default, not-first-
        // layer case -- without the exemption this is the one that silently
        // never records.
        CHECK_FALSE(wouldTakeHiddenSkipPath(HideableKind::Terminal, true, true, true, false));
        CHECK_FALSE(wouldTakeHiddenSkipPath(HideableKind::Middle, true, true, true, false));
    }
    TEST_CASE("a hidden hideable pass that owns its RT's clear still never skips either way") {
        // clears_output=true already made IsHiddenPassSkippable false on its
        // own (the private-offscreen-RT case) -- the exemption changes
        // nothing there, it's only load-bearing for clears_output=false.
        CHECK_FALSE(wouldTakeHiddenSkipPath(HideableKind::Terminal, true, true, true, true));
        CHECK_FALSE(wouldTakeHiddenSkipPath(HideableKind::None, true, true, true, true));
    }
    // execute() feeds a Middle pass HiddenEffectMustCopy; "H" is a hidden
    // effect, "V" a visible one.
    TEST_CASE("a hidden middle effect that breaks the chain parity records its copy") {
        const std::vector<bool> chain { false, true }; // HV
        const bool              must = wallpaper::vulkan::HiddenEffectMustCopy(0, chain, true);
        CHECK(must);
        CHECK_FALSE(wouldTakeHiddenSkipPath(HideableKind::Middle, true, true, true, false, must));
    }
    TEST_CASE("a hidden middle effect that keeps the parity takes the skip") {
        const std::vector<bool> chain { false, false, true }; // HHV
        const bool              must = wallpaper::vulkan::HiddenEffectMustCopy(0, chain, true);
        CHECK_FALSE(must);
        CHECK(wouldTakeHiddenSkipPath(HideableKind::Middle, true, true, true, false, must));
    }
    TEST_CASE("a hidden middle effect in a hidden layer takes the skip") {
        const std::vector<bool> chain { false, true };
        const bool              must = wallpaper::vulkan::HiddenEffectMustCopy(0, chain, true);
        CHECK(wouldTakeHiddenSkipPath(HideableKind::Middle, true, false, true, false, must));
    }

    TEST_CASE("a hideable pass inside a hidden layer takes the skip") {
        CHECK(wouldTakeHiddenSkipPath(HideableKind::Terminal, true, false, true, false));
        CHECK(wouldTakeHiddenSkipPath(HideableKind::Terminal, false, false, true, false));
    }
}

// A terminal-hideable pass appends 2 extra texture reads (the chain's two
// ping-pong buffers) on top of however many the real material already
// declares, so an index built from vk_textures.size() can run past
// WE_GLTEX_NAMES' fixed 13 entries -- Wallpaper Engine materials only ever
// declare g_Texture0 through g_Texture12. GLTexNameForIndex is the
// bounds-checked stand-in for indexing that array directly.
TEST_SUITE("CustomShaderPass::IsAppendedPassthroughSlot") {
    using wallpaper::vulkan::IsAppendedPassthroughSlot;

    TEST_CASE("a pass without a passthrough has no appended slots") {
        CHECK_FALSE(IsAppendedPassthroughSlot(0, -1, -1));
        CHECK_FALSE(IsAppendedPassthroughSlot(5, -1, -1));
    }
    TEST_CASE("a middle pass's single slot and anything after it are appended") {
        CHECK_FALSE(IsAppendedPassthroughSlot(1, 2, -1));
        CHECK(IsAppendedPassthroughSlot(2, 2, -1));
        CHECK(IsAppendedPassthroughSlot(3, 2, -1));
    }
    TEST_CASE("a terminal's pair starts at the lower index") {
        CHECK_FALSE(IsAppendedPassthroughSlot(0, 1, 2));
        CHECK(IsAppendedPassthroughSlot(1, 1, 2));
        CHECK(IsAppendedPassthroughSlot(2, 2, 1));
    }
    TEST_CASE("index 0 is a valid passthrough slot and the lower of the pair still wins") {
        // A later, larger index must not replace a first index of 0.
        CHECK(IsAppendedPassthroughSlot(1, 0, 3));
        CHECK(IsAppendedPassthroughSlot(0, 0, 3));
        CHECK(IsAppendedPassthroughSlot(1, 3, 0));
    }
}

TEST_SUITE("CustomShaderPass::GLTexNameForIndex") {
    using wallpaper::vulkan::GLTexNameForIndex;

    TEST_CASE("in-range indices return the matching g_TextureN name") {
        CHECK(std::string_view(GLTexNameForIndex(0)) == "g_Texture0");
        CHECK(std::string_view(GLTexNameForIndex(12)) == "g_Texture12");
    }
    TEST_CASE("the first index past the table returns null, not a read past the end") {
        // A material that already declares all 13 slots (0..12) plus this
        // pass's 2 passthrough reads lands exactly here (index 13) --
        // without the bound this indexed WE_GLTEX_NAMES out of range.
        CHECK(GLTexNameForIndex(13) == nullptr);
    }
    TEST_CASE("further past the table also returns null") {
        CHECK(GLTexNameForIndex(14) == nullptr);
        CHECK(GLTexNameForIndex(100) == nullptr);
    }
}

TEST_SUITE("CustomShaderPass fallback texture") {
    using wallpaper::i32;
    using wallpaper::vulkan::ImageSlotsRef;
    using wallpaper::vulkan::NeedsFallbackTexture;

    TEST_CASE("a binding the shader never declared (-1) needs no fallback") {
        std::vector<i32>           bindings { -1 };
        std::vector<ImageSlotsRef> textures(1);
        CHECK_FALSE(NeedsFallbackTexture(bindings, textures));
    }
    TEST_CASE("a shader-declared binding with no slots wired needs the fallback") {
        std::vector<i32>           bindings { 0 };
        std::vector<ImageSlotsRef> textures(1);
        CHECK(NeedsFallbackTexture(bindings, textures));
    }
    TEST_CASE("a bound slot that already has an image needs no fallback") {
        std::vector<i32>           bindings { 0 };
        std::vector<ImageSlotsRef> textures(1);
        textures[0].slots.emplace_back();
        CHECK_FALSE(NeedsFallbackTexture(bindings, textures));
    }
    TEST_CASE("a later index in a multi-texture list is still scanned") {
        // Index 0 is already satisfied; only index 1 needs the fallback. A
        // scan that stops after one entry would miss it.
        std::vector<i32>           bindings { 0, 1 };
        std::vector<ImageSlotsRef> textures(2);
        textures[0].slots.emplace_back();
        CHECK(NeedsFallbackTexture(bindings, textures));
    }
}

// The chain alternates two buffers: effect i writes B for even i, A for odd,
// and the layer's own image is drawn into A. A hidden effect must copy only
// when it ends a hidden run and the run leaves the next visible effect
// reading the wrong buffer.
TEST_SUITE("CustomShaderPass::HiddenEffectMustCopy") {
    using wallpaper::usize;
    using wallpaper::vulkan::HiddenEffectMustCopy;
    using wallpaper::vulkan::NearestVisibleBefore;

    // 'V'/'v' visible, 'H' hidden; returns the indices that must copy.
    std::vector<usize> copies(const char* pattern, bool baseWritten = true) {
        std::vector<bool> vis;
        for (const char* p = pattern; *p; p++) vis.push_back(*p != 'H');
        std::vector<usize> out;
        for (usize i = 0; i < vis.size(); i++)
            if (HiddenEffectMustCopy(i, vis, baseWritten)) out.push_back(i);
        return out;
    }

    TEST_CASE("nearest visible before: -1 means the layer's own image") {
        CHECK(NearestVisibleBefore(0, { false, true }) == -1);
        CHECK(NearestVisibleBefore(2, { true, false, false }) == 0);
        CHECK(NearestVisibleBefore(3, { false, true, false, true }) == 1);
    }
    TEST_CASE("a hidden first effect before a visible one copies the layer image") {
        CHECK(copies("HV") == std::vector<usize> { 0 });
    }
    TEST_CASE("an even hidden run already leaves the right buffer") {
        CHECK(copies("HHV").empty());
        CHECK(copies("VHHV").empty());
    }
    TEST_CASE("an odd hidden run copies once, at its end") {
        CHECK(copies("HHHV") == std::vector<usize> { 2 });
        CHECK(copies("VHV") == std::vector<usize> { 1 });
        CHECK(copies("VHHHV") == std::vector<usize> { 3 });
    }
    TEST_CASE("alternating patterns copy at every odd run") {
        CHECK(copies("vHVHV") == (std::vector<usize> { 1, 3 }));
        CHECK(copies("HVHV") == (std::vector<usize> { 0, 2 }));
    }
    TEST_CASE("a hidden last effect never copies; it picks its own source") {
        CHECK(copies("VHH").empty());
        CHECK(copies("HHH").empty());
        CHECK(copies("H").empty());
    }
    TEST_CASE("a visible effect never copies") { CHECK(copies("VVV").empty()); }
    TEST_CASE("no copy of the layer image when nothing draws it") {
        CHECK(copies("HV", /*baseWritten=*/false).empty());
        CHECK(copies("HHHV", /*baseWritten=*/false).empty());
        CHECK(copies("VHV", /*baseWritten=*/false) == std::vector<usize> { 1 });
    }
}

TEST_SUITE("CustomShaderPass::ChainVisibility") {
    TEST_CASE("reads each effect's own flag, treating a missing effect as hidden") {
        wallpaper::SceneImageEffect a, b;
        a.visible = true;
        b.visible = false;
        auto v = wallpaper::vulkan::ChainVisibility({ &a, nullptr, &b });
        CHECK(v == std::vector<bool> { true, false, false });
    }
    TEST_CASE("every entry is filled, not just the first") {
        wallpaper::SceneImageEffect a, b;
        a.visible = false;
        b.visible = true;
        CHECK(wallpaper::vulkan::ChainVisibility({ &a, &b }) == std::vector<bool> { false, true });
    }
    TEST_CASE("ChainVisibilityInto gives the same answer into a reused buffer") {
        wallpaper::SceneImageEffect a, b;
        a.visible = true;
        b.visible = false;
        std::vector<bool> v;
        wallpaper::vulkan::ChainVisibilityInto({ &a, nullptr, &b }, v);
        CHECK(v == wallpaper::vulkan::ChainVisibility({ &a, nullptr, &b }));
        const auto cap = v.capacity();
        a.visible      = false;
        wallpaper::vulkan::ChainVisibilityInto({ &a, &b }, v);
        CHECK(v == std::vector<bool> { false, false });
        CHECK(v.capacity() == cap);
    }
}

#include "Vulkan/TextureCache.hpp"
#include "VulkanRender/CopyPass.hpp"

// A pass keeps a copy of its render targets' VkImage/VkImageView handles for
// the command stream.  Those copies are refreshed from the slot whenever the
// slot's generation moved or its image appeared or vanished, each input
// against its own record: a terminal writes _rt_default but samples the
// chain's ping-pongs through its appended reads, so an output-keyed refresh
// would leave it sampling a dead image.
TEST_SUITE("CustomShaderPass slot rebind") {
    using wallpaper::vulkan::AbsentImageParameters;
    using wallpaper::vulkan::BoundImage;
    using wallpaper::vulkan::BoundImageStale;
    using wallpaper::vulkan::RtSlot;
    using wallpaper::vulkan::TextureKey;

    static void attach(RtSlot & s, std::uint32_t w, std::uint32_t h) {
        s.image.emplace();
        s.image->extent       = { w, h, 1 };
        s.image->mipmap_level = 1;
        ++s.generation;
    }

    TEST_CASE("Desc defaults carry no slots") {
        CustomShaderPass::Desc d;
        CHECK(d.out_slot == nullptr);
        CHECK(d.in_slots.empty());
        wallpaper::vulkan::CopyPass::Desc c;
        CHECK(c.src_slot == nullptr);
        CHECK(c.dst_slot == nullptr);
    }
    TEST_CASE(
        "a CopyPass refreshes both sides from their slots, absent sides keep the key extent") {
        RtSlot src, dst;
        src.key = { .width        = 16,
                    .height       = 8,
                    .usage        = {},
                    .format       = wallpaper::TextureFormat::RGBA8,
                    .sample       = {},
                    .mipmap_level = 1 };
        dst.key = src.key;
        // The attached extent differs from the key, so a rebind that did
        // nothing could not pass: 16x8 would stay.
        attach(src, 32, 4);
        wallpaper::vulkan::CopyPass::Desc c;
        c.src_slot = &src;
        c.dst_slot = &dst;
        // prepare() seeds the handle copies from the key; this Desc skipped
        // prepare, so seed them the same way.
        c.vk_src = AbsentImageParameters(src.key);
        c.vk_dst = AbsentImageParameters(dst.key);
        wallpaper::vulkan::CopyPass copy(c);
        copy.rebindImages();
        CHECK(copy.desc().vk_src.handle == *src.image->handle);
        CHECK(copy.desc().vk_src.extent.width == 32);
        CHECK(copy.desc().vk_src.extent.height == 4);
        CHECK(copy.desc().vk_dst.handle == VK_NULL_HANDLE);
        CHECK(copy.desc().vk_dst.extent.width == 16);
        CHECK(copy.desc().vk_dst.extent.height == 8);
        attach(dst, 32, 4);
        copy.rebindImages();
        CHECK(copy.desc().vk_dst.extent.width == 32);
        CHECK(copy.desc().vk_dst.extent.height == 4);
        // Released: the handle goes and the extent falls back to the key's.
        src.image.reset();
        copy.rebindImages();
        CHECK(copy.desc().vk_src.handle == VK_NULL_HANDLE);
        CHECK(copy.desc().vk_src.extent.width == 16);
        CHECK(copy.desc().vk_src.extent.height == 8);
    }
    TEST_CASE("a bound record is stale when the generation moved or residency flipped") {
        RtSlot     s;
        BoundImage b;
        CHECK_FALSE(BoundImageStale(b, s)); // both absent at generation 0
        attach(s, 8, 8);
        CHECK(BoundImageStale(b, s));
        b = { s.generation, s.resident() };
        CHECK_FALSE(BoundImageStale(b, s));
        s.image.reset(); // released: same generation, no image
        CHECK(BoundImageStale(b, s));
    }
    TEST_CASE("absent parameters keep the extent and mip count the pipelines were built for") {
        TextureKey key { .width        = 1920,
                         .height       = 1080,
                         .usage        = {},
                         .format       = wallpaper::TextureFormat::RGBA8,
                         .sample       = {},
                         .mipmap_level = 3 };
        auto       p = AbsentImageParameters(key);
        CHECK(p.handle == VK_NULL_HANDLE);
        CHECK(p.view == VK_NULL_HANDLE);
        CHECK(p.extent.width == 1920);
        CHECK(p.extent.height == 1080);
        CHECK(p.mipmap_level == 3);
    }
    TEST_CASE("paramsOrAbsent gives the attached image, or the key's extent once it is released") {
        RtSlot s;
        s.key = { .width        = 64,
                  .height       = 32,
                  .usage        = {},
                  .format       = wallpaper::TextureFormat::RGBA8,
                  .sample       = {},
                  .mipmap_level = 1 };
        attach(s, 128, 16);
        CHECK(s.paramsOrAbsent().extent.width == 128);
        CHECK(s.paramsOrAbsent().extent.height == 16);
        s.image.reset();
        CHECK(s.paramsOrAbsent().handle == VK_NULL_HANDLE);
        CHECK(s.paramsOrAbsent().extent.width == 64);
        CHECK(s.paramsOrAbsent().extent.height == 32);
    }
    TEST_CASE("rebind refreshes the output and every managed input, including the appended "
              "passthrough reads, and leaves bitmap slots alone") {
        RtSlot out, a, b;
        out.key = { .width        = 64,
                    .height       = 64,
                    .usage        = {},
                    .format       = wallpaper::TextureFormat::RGBA8,
                    .sample       = {},
                    .mipmap_level = 1 };
        a.key = b.key = { .width        = 32,
                          .height       = 32,
                          .usage        = {},
                          .format       = wallpaper::TextureFormat::RGBA8,
                          .sample       = {},
                          .mipmap_level = 1 };
        // Every attached extent differs from its key, so a rebind that did
        // nothing leaves the key extents and fails the checks below.
        attach(out, 128, 32);
        attach(a, 16, 48);
        attach(b, 16, 48);

        CustomShaderPass::Desc d;
        d.textures = { "bitmap.tex", "_rt_a", "_rt_b" };
        d.vk_textures.resize(3);
        d.vk_textures[0].slots.resize(1); // a bitmap texture, not a slot
        d.vk_textures[0].slots[0].extent = { 7, 7, 1 };
        d.in_slots                       = { nullptr, &a, &b };
        d.out_slot                       = &out;
        d.hideable                       = wallpaper::vulkan::HideableKind::Terminal;
        d.passthrough_input_tex_index_a  = 1;
        d.passthrough_input_tex_index_b  = 2;
        CustomShaderPass pass(d);
        pass.rebindImages();
        CHECK(pass.desc().vk_output.handle == *out.image->handle);
        CHECK(pass.desc().vk_output.extent.width == 128);
        CHECK(pass.desc().vk_output.extent.height == 32);
        REQUIRE(pass.desc().vk_textures[1].slots.size() == 1);
        REQUIRE(pass.desc().vk_textures[2].slots.size() == 1);
        CHECK(pass.desc().vk_textures[1].slots[0].extent.width == 16);
        CHECK(pass.desc().vk_textures[0].slots[0].extent.width == 7);

        // Only b moves: a keeps its copy, b's copy follows the new image.
        attach(b, 48, 48);
        pass.rebindImages();
        CHECK(pass.desc().vk_textures[1].slots[0].extent.width == 16);
        CHECK(pass.desc().vk_textures[2].slots[0].extent.width == 48);

        // b released: its copy is dropped, so a non-drawing record binds nothing for it.
        b.image.reset();
        pass.rebindImages();
        CHECK(pass.desc().vk_textures[2].slots.empty());
        CHECK(pass.desc().vk_textures[1].slots.size() == 1);

        // The output released: the handles go, the extent falls back to the key's.
        out.image.reset();
        pass.rebindImages();
        CHECK(pass.desc().vk_output.handle == VK_NULL_HANDLE);
        CHECK(pass.desc().vk_output.extent.width == 64);
        CHECK(pass.desc().vk_output.extent.height == 64);
    }
    TEST_CASE("a second rebind with the same generation leaves the refreshed copy alone") {
        RtSlot in;
        in.key = { .width        = 32,
                   .height       = 32,
                   .usage        = {},
                   .format       = wallpaper::TextureFormat::RGBA8,
                   .sample       = {},
                   .mipmap_level = 1 };
        attach(in, 16, 16);

        CustomShaderPass::Desc d;
        d.textures = { "_rt_a" };
        d.vk_textures.resize(1);
        d.in_slots = { &in };
        CustomShaderPass pass(d);

        ++in.generation; // the image was replaced
        in.image->extent = { 40, 40, 1 };
        pass.rebindImages();
        CHECK(pass.desc().vk_textures[0].slots[0].extent.width == 40);

        // Same generation: the pass already holds this one and must not re-copy.
        in.image->extent = { 99, 99, 1 };
        pass.rebindImages();
        CHECK(pass.desc().vk_textures[0].slots[0].extent.width == 40);
    }
}

// The framebuffer is built over the output image's view the first time the
// pass records, and again whenever the slot's image changed; prepare() no
// longer needs the image to exist.
TEST_SUITE("CustomShaderPass framebuffer rebuild") {
    using wallpaper::vulkan::FramebufferNeedsRebuild;
    using wallpaper::vulkan::RtSlot;

    TEST_CASE("no framebuffer yet: rebuild only once the output has an image") {
        RtSlot out;
        CHECK_FALSE(FramebufferNeedsRebuild(false, 0, out));
        out.image.emplace();
        ++out.generation;
        CHECK(FramebufferNeedsRebuild(false, 0, out));
    }
    TEST_CASE("a framebuffer built for the current generation is kept") {
        RtSlot out;
        out.image.emplace();
        out.generation = 3;
        CHECK_FALSE(FramebufferNeedsRebuild(true, 3, out));
        CHECK(FramebufferNeedsRebuild(true, 2, out));
    }
    TEST_CASE("a replaced framebuffer goes to the retirer, and only when there is one") {
        using wallpaper::vulkan::RetireOldFramebuffer;
        std::vector<VkFramebuffer>  retired;
        std::function<void(FakeFb)> retirer = [&](FakeFb fb) {
            retired.push_back(fb.h);
        };
        FakeFb old { fb(7) };
        CHECK(RetireOldFramebuffer(old, retirer));
        CHECK(retired == std::vector<VkFramebuffer> { fb(7) });
        FakeFb none_built { VK_NULL_HANDLE };
        CHECK_FALSE(RetireOldFramebuffer(none_built, retirer)); // nothing was built yet
        CHECK(retired.size() == 1);
        std::function<void(FakeFb)> none;
        old = FakeFb { fb(9) };
        CHECK_FALSE(RetireOldFramebuffer(old, none)); // caller destroys in place
        CHECK(old.h == fb(9));
    }
    TEST_CASE("a member or end of a group whose opener could not begin its render pass skips") {
        using wallpaper::vulkan::GroupMemberSkips;
        using wallpaper::vulkan::PassMergeRole;
        CHECK(GroupMemberSkips(PassMergeRole::GroupMiddle, true));
        CHECK(GroupMemberSkips(PassMergeRole::GroupEnd, true));
        CHECK_FALSE(GroupMemberSkips(PassMergeRole::GroupBegin, true)); // it decides, not follows
        CHECK_FALSE(GroupMemberSkips(PassMergeRole::Standalone, true)); // never inside a group
        CHECK_FALSE(GroupMemberSkips(PassMergeRole::GroupMiddle, false));
        CHECK_FALSE(GroupMemberSkips(PassMergeRole::GroupEnd, false));
    }
    TEST_CASE("a framebuffer failure logs once per streak and a success re-arms it") {
        using wallpaper::vulkan::ShouldLogFramebufferFailure;
        bool logged = false;
        CHECK_FALSE(ShouldLogFramebufferFailure(false, logged));
        CHECK(ShouldLogFramebufferFailure(true, logged));
        CHECK_FALSE(ShouldLogFramebufferFailure(true, logged));
        CHECK_FALSE(ShouldLogFramebufferFailure(true, logged));
        CHECK_FALSE(ShouldLogFramebufferFailure(false, logged));
        CHECK(ShouldLogFramebufferFailure(true, logged));
    }
}

// Every visibility read in execute() and the per-frame demand walk go through
// one decision, so what a pass records, whether it draws and which buffers it
// samples can never disagree between the two.
TEST_SUITE("CustomShaderPass::DecideHideable") {
    using wallpaper::i32;
    using wallpaper::vulkan::DecideHideable;
    using wallpaper::vulkan::DemandInputs;
    using wallpaper::vulkan::HideableFrame;
    using wallpaper::vulkan::HideableKind;
    using wallpaper::vulkan::PassMergeRole;

    // Defaults: a node that is visible with visible ancestors, passthrough
    // built, not the clear owner, standalone, passthrough reads at 5 and 6.
    static HideableFrame decide(HideableKind kind,
                                std::vector<bool>
                                              chain,
                                i32           index,
                                bool          node_visible,
                                bool          ancestors_visible = true,
                                bool          passthrough_ready = true,
                                bool          clears_output     = false,
                                PassMergeRole role              = PassMergeRole::Standalone) {
        return DecideHideable(kind,
                              chain,
                              index,
                              /*has_node=*/true,
                              node_visible,
                              ancestors_visible,
                              /*base_image_written=*/true,
                              passthrough_ready,
                              clears_output,
                              role,
                              5,
                              6);
    }

    TEST_CASE("a chain index equal to the chain length is unknown, not read past the end") {
        // An unknown chain position follows the node.
        std::vector<bool> chain { true };
        auto              f = decide(HideableKind::Middle, chain, 1, true);
        CHECK_FALSE(f.own_hidden);
        CHECK(f.draws);
    }
    TEST_CASE("an ordinary visible pass records, draws and samples its own textures") {
        auto f = decide(HideableKind::None, {}, -1, true);
        CHECK(f.records);
        CHECK(f.draws);
        CHECK(f.inputs == DemandInputs::Own);
        CHECK(f.passthrough_tex_index == -1);
    }
    TEST_CASE("a pass without a node always draws") {
        auto f = DecideHideable(HideableKind::None,
                                {},
                                -1,
                                false,
                                false,
                                false,
                                true,
                                false,
                                false,
                                PassMergeRole::Standalone,
                                -1,
                                -1);
        CHECK(f.node_visible);
        CHECK(f.draws);
        CHECK(f.inputs == DemandInputs::Own);
    }
    TEST_CASE("a hidden ordinary pass that owns nothing drops its whole record") {
        auto f = decide(HideableKind::None, {}, -1, false);
        CHECK_FALSE(f.records);
        CHECK_FALSE(f.draws);
        CHECK(f.inputs == DemandInputs::None);
    }
    TEST_CASE("a hidden clear owner records without drawing and samples nothing") {
        auto f = decide(HideableKind::None, {}, -1, false, true, true, /*clears_output=*/true);
        CHECK(f.records);
        CHECK_FALSE(f.draws);
        CHECK(f.inputs == DemandInputs::None);
    }
    TEST_CASE("a hidden pass that opens a merged group records the boundary without drawing") {
        auto f =
            decide(HideableKind::None, {}, -1, false, true, true, false, PassMergeRole::GroupBegin);
        CHECK(f.records);
        CHECK_FALSE(f.draws);
        CHECK(f.inputs == DemandInputs::None);
    }
    TEST_CASE("a visible terminal draws its material and reads its own textures only") {
        auto f = decide(HideableKind::Terminal, { true, true }, 1, true);
        CHECK(f.draws);
        CHECK_FALSE(f.hide.draw_passthrough);
        CHECK(f.inputs == DemandInputs::Own);
    }
    TEST_CASE("a hidden terminal behind a visible effect copies buffer B") {
        auto f = decide(HideableKind::Terminal, { true, false }, 1, false);
        CHECK(f.own_hidden);
        CHECK(f.hide.draw_passthrough);
        CHECK(f.draws);
        CHECK(f.inputs == DemandInputs::PassthroughB);
        CHECK(f.passthrough_tex_index == 6);
    }
    TEST_CASE("a hidden terminal behind hidden effects copies the layer image in buffer A") {
        auto f = decide(HideableKind::Terminal, { false, false }, 1, false);
        CHECK(f.inputs == DemandInputs::PassthroughA);
        CHECK(f.passthrough_tex_index == 5);
    }
    TEST_CASE("a hidden terminal with no passthrough pipeline keeps its real draw") {
        auto f = decide(HideableKind::Terminal, { true, false }, 1, false, true, /*ready=*/false);
        CHECK(f.hide.keep_real_draw);
        CHECK(f.draws);
        CHECK(f.inputs == DemandInputs::Own);
    }
    TEST_CASE("a terminal inside a hidden layer copies nothing and drops its record") {
        auto f = decide(HideableKind::Terminal, { true, false }, 1, false, /*ancestors=*/false);
        CHECK_FALSE(f.hide.draw_passthrough);
        CHECK_FALSE(f.records);
    }
    TEST_CASE("a hidden middle effect that must copy reads its chain input") {
        auto f = decide(HideableKind::Middle, { false, true }, 0, false);
        CHECK(f.must_copy);
        CHECK(f.hide.draw_passthrough);
        CHECK(f.inputs == DemandInputs::PassthroughA);
        CHECK(f.passthrough_tex_index == 5);
    }
    TEST_CASE("a hidden middle effect that keeps the parity takes the hidden path") {
        auto skip = decide(HideableKind::Middle, { false, false }, 0, false);
        CHECK_FALSE(skip.must_copy);
        CHECK_FALSE(skip.records);
        auto clear_owner =
            decide(HideableKind::Middle, { false, false }, 0, false, true, true, true);
        CHECK(clear_owner.records);
        CHECK_FALSE(clear_owner.draws);
        CHECK(clear_owner.inputs == DemandInputs::None);
    }
    TEST_CASE("frameHideable on a node-less pass is the plain draw") {
        CustomShaderPass::Desc d;
        CustomShaderPass       pass(d);
        auto                   f = pass.frameHideable(PassMergeRole::Standalone);
        CHECK(f.draws);
        CHECK(f.inputs == DemandInputs::Own);
    }
    TEST_CASE("every kind x chain shape x visibility x passthrough_ready x clear x role obeys the "
              "invariants execute() relies on") {
        // Exhaustive rather than sampled: the demand walk trusts these four
        // relations for every combination, so every combination is checked.
        const HideableKind                   kinds[] = { HideableKind::None,
                                                         HideableKind::Middle,
                                                         HideableKind::Terminal };
        const std::vector<std::vector<bool>> chains  = { {},
                                                         { true },
                                                         { false },
                                                         { true, true },
                                                         { true, false },
                                                         { false, true },
                                                         { false, false },
                                                         { true, false, true },
                                                         { false, false, false } };
        const PassMergeRole                  roles[] = { PassMergeRole::Standalone,
                                                         PassMergeRole::GroupBegin,
                                                         PassMergeRole::GroupMiddle,
                                                         PassMergeRole::GroupEnd };
        int                                  combos  = 0;
        for (HideableKind kind : kinds)
            for (const auto& chain : chains)
                for (i32 index = -1; index < (i32)chain.size(); ++index)
                    for (bool node_visible : { true, false })
                        for (bool ancestors : { true, false })
                            for (bool ready : { true, false })
                                for (bool clears : { true, false })
                                    for (bool base_written : { true, false })
                                        for (PassMergeRole role : roles) {
                                            ++combos;
                                            const auto f = DecideHideable(kind,
                                                                          chain,
                                                                          index,
                                                                          true,
                                                                          node_visible,
                                                                          ancestors,
                                                                          base_written,
                                                                          ready,
                                                                          clears,
                                                                          role,
                                                                          5,
                                                                          6);
                                            INFO("kind=" << (int)kind << " index=" << index
                                                         << " node_visible=" << node_visible
                                                         << " ancestors=" << ancestors << " ready="
                                                         << ready << " clears=" << clears
                                                         << " role=" << (int)role);
                                            // 1. a draw is always recorded
                                            if (f.draws) CHECK(f.records);
                                            // 2. draws == visible || one of the two hidden draws
                                            CHECK(f.draws ==
                                                  (f.node_visible || f.hide.draw_passthrough ||
                                                   f.hide.keep_real_draw));
                                            // 3. inputs None exactly when nothing is drawn
                                            CHECK((f.inputs == DemandInputs::None) == ! f.draws);
                                            // 4. a passthrough names its buffer, Own names none
                                            const bool pt =
                                                f.inputs == DemandInputs::PassthroughA ||
                                                f.inputs == DemandInputs::PassthroughB;
                                            CHECK(pt == (f.passthrough_tex_index != -1));
                                            if (pt) CHECK(f.hide.draw_passthrough);
                                            // 5. a hidden pass drops its record only on a role
                                            //    that may drop it
                                            if (! f.records) CHECK(HiddenPassDropsRecording(role));
                                            // 6. a visible node never takes a hidden draw
                                            if (f.node_visible && ! f.own_hidden)
                                                CHECK_FALSE(f.hide.draw_passthrough);
                                        }
        CHECK(combos > 3000);
    }
}

// The per-frame half of a pass's demand comes from its HideableFrame; cached
// bytes depend on the images they were drawn from, so a cached pass keeps its
// own inputs resident even on a frame it records nothing.
TEST_SUITE("CustomShaderPass::DemandFrameFor") {
    using wallpaper::vulkan::DemandFrameFor;
    using wallpaper::vulkan::DemandInputs;
    using wallpaper::vulkan::HideableFrame;
    TEST_CASE("a drawing pass passes its inputs through") {
        HideableFrame hf;
        hf.draws  = true;
        hf.inputs = DemandInputs::PassthroughB;
        auto f    = DemandFrameFor(hf, false);
        CHECK(f.draws);
        CHECK_FALSE(f.cached);
        CHECK(f.inputs == DemandInputs::PassthroughB);
    }
    TEST_CASE("a cached pass that records nothing this frame still needs its own inputs") {
        HideableFrame hf; // records=true, draws=false, inputs=None by default
        auto          f = DemandFrameFor(hf, true);
        CHECK_FALSE(f.draws);
        CHECK(f.cached);
        CHECK(f.inputs == DemandInputs::Own);
    }
    TEST_CASE("an uncached non-drawing pass needs nothing") {
        HideableFrame hf;
        auto          f = DemandFrameFor(hf, false);
        CHECK(f.inputs == DemandInputs::None);
    }
}

// One pass's compile-time demand entry: what it writes, the slots its own
// textures sample, and the two chain buffers a hidden effect may copy from.
TEST_SUITE("CustomShaderPass::PassDemandStaticFor") {
    using wallpaper::vulkan::PassDemandStaticFor;
    using wallpaper::vulkan::ResidencyKey;
    using wallpaper::vulkan::RtSlot;
    using K = ResidencyKey;

    TEST_CASE("a terminal's two appended passthrough reads are not own inputs") {
        // RtSlot is neither copyable nor movable, so the ids are assigned in place.
        RtSlot out, tex, a, b;
        out.id      = 9;
        tex.id      = 1;
        a.id        = 2;
        b.id        = 3;
        out.persist = true; // writes _rt_default
        CustomShaderPass::Desc d;
        d.textures                      = { "bitmap.tex", "_rt_tex", "_rt_a", "_rt_b" };
        d.in_slots                      = { nullptr, &tex, &a, &b };
        d.out_slot                      = &out;
        d.passthrough_input_tex_index_a = 2;
        d.passthrough_input_tex_index_b = 3;
        const auto s                    = PassDemandStaticFor(d);
        CHECK(s.out == K::ForSlot(9));
        CHECK_FALSE(s.out_managed);
        CHECK(s.own_inputs == std::vector<K> { K::ForSlot(1) });
        CHECK(s.passthrough_a == K::ForSlot(2));
        CHECK(s.passthrough_b == K::ForSlot(3));
    }
    TEST_CASE("a middle effect has one passthrough read; b stays null") {
        RtSlot out, in;
        out.id = 5;
        in.id  = 4;
        CustomShaderPass::Desc d;
        d.textures                      = { "_rt_in" };
        d.in_slots                      = { &in };
        d.out_slot                      = &out;
        d.passthrough_input_tex_index_a = 0;
        d.passthrough_input_tex_index_b = -1;
        const auto s                    = PassDemandStaticFor(d);
        CHECK(s.out_managed);
        CHECK(s.own_inputs.empty());
        CHECK(s.passthrough_a == K::ForSlot(4));
        CHECK(s.passthrough_b == K {});
    }
    TEST_CASE("a passthrough index equal to the slot count is out of range") {
        CustomShaderPass::Desc d;
        d.in_slots                      = { nullptr, nullptr };
        d.passthrough_input_tex_index_a = 2;
        d.passthrough_input_tex_index_b = 2;
        const auto s                    = PassDemandStaticFor(d);
        CHECK(s.passthrough_a == K {});
        CHECK(s.passthrough_b == K {});
    }
    TEST_CASE(
        "an unbound slot and a pass with no output give null keys, never a read past the end") {
        CustomShaderPass::Desc d;
        d.textures                      = { "a", "b" };
        d.in_slots                      = { nullptr, nullptr };
        d.passthrough_input_tex_index_a = 7; // out of range: ignored
        const auto s                    = PassDemandStaticFor(d);
        CHECK(s.out == K {});
        CHECK_FALSE(s.out_managed);
        CHECK(s.own_inputs.empty());
        CHECK(s.passthrough_a == K {});
    }
}

TEST_SUITE("CustomShaderPass blocked frame") {
    TEST_CASE("a blocked pass takes the hidden path: no draw, no sampled inputs") {
        CustomShaderPass::Desc d;
        CustomShaderPass       pass(d);
        pass.setBlockedThisFrame(true);
        auto f = pass.frameHideable(wallpaper::vulkan::PassMergeRole::Standalone);
        CHECK_FALSE(f.draws);
        CHECK(f.inputs == wallpaper::vulkan::DemandInputs::None);
        CHECK_FALSE(pass.demandFrame().draws);
        // The flag is per frame: once residencyStep clears it, the next
        // frame's demand sees the pass drawing again, so the slot it was
        // waiting for stays needed and is retried, instead of being judged
        // unneeded and the pass unblocked onto an absent input.
        pass.setBlockedThisFrame(false);
        CHECK(pass.frameHideable(wallpaper::vulkan::PassMergeRole::Standalone).draws);
        CHECK(pass.demandFrame().draws);
    }
}

// The guard at the bind site: a drawing pass must never bind the opaque 1x1
// fallback where a managed render target should be.  rebindImages empties
// the texture's slot list exactly when the slot is absent, so the check
// belongs to the empty-slots branch.
TEST_SUITE("CustomShaderPass absent managed input") {
    using wallpaper::vulkan::AbsentManagedInputBlocksDraw;
    using wallpaper::vulkan::RtSlot;
    TEST_CASE("a bitmap texture (no slot) never blocks") {
        CHECK_FALSE(AbsentManagedInputBlocksDraw(nullptr, true));
    }
    TEST_CASE("an absent managed slot blocks a drawing pass only") {
        RtSlot s;
        CHECK(AbsentManagedInputBlocksDraw(&s, true));
        CHECK_FALSE(AbsentManagedInputBlocksDraw(&s, false)); // a non-drawing record binds nothing
    }
    TEST_CASE("a resident slot or a persistent slot never blocks") {
        RtSlot s;
        s.image.emplace();
        CHECK_FALSE(AbsentManagedInputBlocksDraw(&s, true));
        RtSlot p;
        p.persist = true;
        CHECK_FALSE(AbsentManagedInputBlocksDraw(&p, true));
    }
    // A record that does not draw (a hidden pass holding a clear, a group
    // member) samples nothing, so it writes no descriptor for an absent input
    // instead of binding the opaque fallback in its place.
    TEST_CASE("a non-drawing record skips the descriptor for an absent managed input only") {
        using wallpaper::vulkan::AbsentManagedInputSkipsBind;
        RtSlot absent;
        CHECK(AbsentManagedInputSkipsBind(&absent, false));
        CHECK_FALSE(AbsentManagedInputSkipsBind(&absent, true)); // the block rule owns this case
        CHECK_FALSE(
            AbsentManagedInputSkipsBind(nullptr, false)); // bitmap texture keeps the fallback
        RtSlot resident;
        resident.image.emplace();
        CHECK_FALSE(AbsentManagedInputSkipsBind(&resident, false));
        RtSlot persistent;
        persistent.persist = true;
        CHECK_FALSE(AbsentManagedInputSkipsBind(&persistent, false));
    }
}

// The out-of-memory log reports the device-local heaps, whichever indices
// they sit at (heap 0 is host memory on some drivers).
TEST_SUITE("Device::SumDeviceLocalHeaps") {
    TEST_CASE("only DEVICE_LOCAL heaps are summed") {
        std::array<wallpaper::vulkan::Device::HeapBudget, VK_MAX_MEMORY_HEAPS> heaps {};
        heaps[0]       = { 100, 1000, 0 }; // host-visible, ignored
        heaps[1]       = { 200, 4000, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT };
        heaps[2]       = { 50, 256, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT }; // BAR window, counted
        const auto sum = wallpaper::vulkan::SumDeviceLocalHeaps(heaps);
        CHECK(sum.usage == 250);
        CHECK(sum.budget == 4256);
        CHECK(sum.flags == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
    }
}

// The copy's destination moves UNDEFINED -> TRANSFER_DST and its source
// SHADER_READ_ONLY -> TRANSFER_SRC.  Both have to wait for whoever last read
// or wrote those images (fragment sampling, a render pass store, an earlier
// copy); TOP_OF_PIPE waits for nothing.
TEST_SUITE("CopyPass barrier source") {
    TEST_CASE("source stage covers fragment reads, attachment writes and transfers") {
        const auto stages = wallpaper::vulkan::CopyBarrierSourceStages();
        // doctest rejects a bare & inside CHECK, so the masks are compared out here.
        CHECK((stages & VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT) != 0);
        CHECK((stages & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) != 0);
        CHECK((stages & VK_PIPELINE_STAGE_TRANSFER_BIT) != 0);
        CHECK((stages & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) == 0);
        const auto access = wallpaper::vulkan::CopyBarrierSourceAccess();
        CHECK((access & VK_ACCESS_MEMORY_READ_BIT) != 0);
        CHECK((access & VK_ACCESS_MEMORY_WRITE_BIT) != 0);
    }
}

// ---------------------------------------------------------------------------
// Decisions execute() and prepare() make that need no device to check.
// ---------------------------------------------------------------------------

TEST_SUITE("CustomShaderPass::MaterialTexBinding") {
    using wallpaper::i32;
    using wallpaper::vulkan::MaterialTexBinding;

    static wallpaper::Map<std::string, VkDescriptorSetLayoutBinding> bindings() {
        wallpaper::Map<std::string, VkDescriptorSetLayoutBinding> m;
        m["g_Texture0"].binding = 1;
        m["g_Texture1"].binding = 2;
        return m;
    }

    TEST_CASE("a declared texture reports its descriptor binding") {
        const auto m = bindings();
        CHECK(MaterialTexBinding(0, -1, -1, m) == 1);
        CHECK(MaterialTexBinding(1, -1, -1, m) == 2);
    }
    TEST_CASE("a texture the shader never declared has no binding") {
        const auto m = bindings();
        CHECK(MaterialTexBinding(2, -1, -1, m) == -1);
    }
    TEST_CASE("an appended passthrough slot stays unbound even if the shader declares it") {
        const auto m = bindings();
        CHECK(MaterialTexBinding(1, 1, -1, m) == -1);
        CHECK(MaterialTexBinding(0, 1, -1, m) == 1);
    }
    TEST_CASE("an index past the name table has no binding") {
        const auto m = bindings();
        CHECK(MaterialTexBinding(20, -1, -1, m) == -1);
    }
}

TEST_SUITE("CustomShaderPass::ResidentSlotParams") {
    using wallpaper::vulkan::ResidentSlotParams;
    using wallpaper::vulkan::RtSlot;

    TEST_CASE("a missing slot and a slot with no image give nothing") {
        CHECK_FALSE(ResidentSlotParams(nullptr).has_value());
        RtSlot s;
        CHECK_FALSE(ResidentSlotParams(&s).has_value());
    }
    TEST_CASE("a slot with an image gives its parameters") {
        RtSlot s;
        s.image.emplace();
        s.image->extent = { 1, 1, 1 };
        auto p          = ResidentSlotParams(&s);
        REQUIRE(p.has_value());
        CHECK(p->extent.width == 1);
        CHECK(p->extent.height == 1);
    }
}

TEST_SUITE("CustomShaderPass vertex attributes") {
    using wallpaper::SceneMesh;
    using wallpaper::SceneVertexArray;
    using wallpaper::vulkan::ShaderReflected;
    using wallpaper::vulkan::VertexAttributeDescriptions;

    static SceneVertexArray array(std::vector<std::string> names) {
        std::vector<SceneVertexArray::SceneVertexAttribute> attrs;
        for (auto& n : names) attrs.push_back({ n, wallpaper::VertexType::FLOAT2 });
        return SceneVertexArray(attrs, 1);
    }
    static wallpaper::Map<std::string, ShaderReflected::Input> inputs() {
        wallpaper::Map<std::string, ShaderReflected::Input> m;
        m["a_Position"] = { 0, VK_FORMAT_R32G32_SFLOAT };
        m["a_TexCoord"] = { 1, VK_FORMAT_R32G32_SFLOAT };
        return m;
    }

    TEST_CASE("one attribute per input for a single vertex array") {
        SceneMesh m;
        m.AddVertexArray(array({ "a_Position", "a_TexCoord" }));
        auto d = VertexAttributeDescriptions(m, inputs());
        REQUIRE(d.size() == 2);
        CHECK(d[0].binding == 0);
        CHECK(d[1].binding == 0);
        CHECK(d[0].location == 0); // a_Position sorts first
        CHECK(d[1].location == 1);
        CHECK(d[0].offset == 0);
        // Attributes are padded, so a_TexCoord does not start right after a_Position's two floats.
        const auto offsets = m.GetVertexArray(0).GetAttrOffsetMap();
        CHECK(d[1].offset == offsets.at("a_TexCoord").offset);
        CHECK(d[1].offset > 0);
    }
    TEST_CASE("every vertex array gets its own binding") {
        SceneMesh m;
        m.AddVertexArray(array({ "a_Position", "a_TexCoord" }));
        m.AddVertexArray(array({ "a_Position", "a_TexCoord" }));
        auto d = VertexAttributeDescriptions(m, inputs());
        REQUIRE(d.size() == 4);
        CHECK(d[0].binding == 0);
        CHECK(d[1].binding == 0);
        CHECK(d[2].binding == 1);
        CHECK(d[3].binding == 1);
    }
    TEST_CASE("an input the array does not carry gets offset 0") {
        SceneMesh m;
        m.AddVertexArray(array({ "a_Position" }));
        auto d = VertexAttributeDescriptions(m, inputs());
        REQUIRE(d.size() == 2);
        CHECK(d[1].location == 1);
        CHECK(d[1].offset == 0);
    }
    TEST_CASE("a mesh with no vertex arrays has no attributes") {
        SceneMesh m;
        CHECK(VertexAttributeDescriptions(m, inputs()).empty());
    }
}

TEST_SUITE("CustomShaderPass::frameHideable chain") {
    using wallpaper::SceneImageEffect;
    using wallpaper::vulkan::HideableKind;
    using wallpaper::vulkan::PassMergeRole;

    TEST_CASE("a hideable pass reads its own effect's flag from the chain") {
        SceneImageEffect a, b;
        a.visible = true;
        b.visible = false;
        CustomShaderPass::Desc d;
        d.hideable      = HideableKind::Terminal;
        d.chain_effects = { &a, &b };
        d.chain_index   = 1;
        CustomShaderPass pass(d);
        CHECK(pass.frameHideable(PassMergeRole::Standalone).own_hidden);
    }
    TEST_CASE("a visible effect in the chain is not hidden") {
        SceneImageEffect a, b;
        a.visible = false;
        b.visible = true;
        CustomShaderPass::Desc d;
        d.hideable      = HideableKind::Terminal;
        d.chain_effects = { &a, &b };
        d.chain_index   = 1;
        CustomShaderPass pass(d);
        CHECK_FALSE(pass.frameHideable(PassMergeRole::Standalone).own_hidden);
    }
}

TEST_SUITE("CustomShaderPass::takeFramebuffer") {
    TEST_CASE("a pass that built no framebuffer has none to give") {
        CustomShaderPass::Desc d;
        CustomShaderPass       pass(d);
        CHECK_FALSE(pass.takeFramebuffer().has_value());
    }
}

TEST_SUITE("CustomShaderPass::DecideFramebufferStep") {
    using wallpaper::vulkan::DecideFramebufferStep;
    using wallpaper::vulkan::FbStep;
    using wallpaper::vulkan::RtSlot;

    static const FakeFb kBuilt { reinterpret_cast<VkFramebuffer>(0x1) };
    static const FakeFb kNone {};

    TEST_CASE("a role that never opens a render pass needs nothing") {
        RtSlot s;
        CHECK(DecideFramebufferStep(false, &s, kNone, 0) == FbStep::Ready);
        CHECK(DecideFramebufferStep(false, nullptr, kNone, 0) == FbStep::Ready);
    }
    TEST_CASE("a pass without a slot is ready exactly when its framebuffer exists") {
        CHECK(DecideFramebufferStep(true, nullptr, kBuilt, 0) == FbStep::Ready);
        CHECK(DecideFramebufferStep(true, nullptr, kNone, 0) == FbStep::Fail);
    }
    TEST_CASE("a slot with no image cannot be built over") {
        RtSlot s;
        CHECK(DecideFramebufferStep(true, &s, kNone, 0) == FbStep::Fail);
        CHECK(DecideFramebufferStep(true, &s, kBuilt, 0) == FbStep::Fail);
    }
    TEST_CASE("an attached slot builds when there is no framebuffer or the image changed") {
        RtSlot s;
        s.image.emplace();
        s.generation = 3;
        CHECK(DecideFramebufferStep(true, &s, kNone, 0) == FbStep::Build);
        CHECK(DecideFramebufferStep(true, &s, kBuilt, 3) == FbStep::Ready);
        ++s.generation;
        CHECK(DecideFramebufferStep(true, &s, kBuilt, 3) == FbStep::Build);
    }
}

TEST_SUITE("CustomShaderPass::FramebufferAttachments") {
    using wallpaper::vulkan::FramebufferAttachments;

    static VkImageView v(std::uintptr_t x) { return reinterpret_cast<VkImageView>(x); }

    TEST_CASE("one sample per pixel attaches the output, then the depth") {
        auto a = FramebufferAttachments(VK_SAMPLE_COUNT_1_BIT, true, v(1), v(2), v(3));
        CHECK(a.count == 2);
        CHECK(a.views[0] == v(2));
        CHECK(a.views[1] == v(3));
        auto b = FramebufferAttachments(VK_SAMPLE_COUNT_1_BIT, false, v(1), v(2), v(3));
        CHECK(b.count == 1);
        CHECK(b.views[0] == v(2));
    }
    TEST_CASE("multisampling attaches the msaa colour, the resolve target, then the depth") {
        auto a = FramebufferAttachments(VK_SAMPLE_COUNT_4_BIT, true, v(1), v(2), v(3));
        CHECK(a.count == 3);
        CHECK(a.views[0] == v(1));
        CHECK(a.views[1] == v(2));
        CHECK(a.views[2] == v(3));
        auto b = FramebufferAttachments(VK_SAMPLE_COUNT_4_BIT, false, v(1), v(2), v(3));
        CHECK(b.count == 2);
        CHECK(b.views[0] == v(1));
        CHECK(b.views[1] == v(2));
    }
}

TEST_SUITE("CustomShaderPass::FramebufferFailureAbandonsGroup") {
    using wallpaper::vulkan::FramebufferFailureAbandonsGroup;
    using wallpaper::vulkan::PassMergeRole;

    TEST_CASE("only the pass that opens a group abandons it") {
        CHECK(FramebufferFailureAbandonsGroup(PassMergeRole::GroupBegin));
        CHECK_FALSE(FramebufferFailureAbandonsGroup(PassMergeRole::Standalone));
        CHECK_FALSE(FramebufferFailureAbandonsGroup(PassMergeRole::GroupMiddle));
        CHECK_FALSE(FramebufferFailureAbandonsGroup(PassMergeRole::GroupEnd));
    }
}

TEST_SUITE("CustomShaderPass::cacheDecision") {
    using wallpaper::vulkan::PassMergeRole;
    using wallpaper::vulkan::RtSlot;

    static std::unique_ptr<CustomShaderPass> cachedPass(RtSlot * slot, std::uint32_t cached_gen) {
        CustomShaderPass::Desc d;
        d.out_slot = slot;
        auto pass  = std::make_unique<CustomShaderPass>(d);
        pass->setCanCache(true);
        pass->markCached(true, cached_gen);
        return pass;
    }

    TEST_CASE("an unchanged cached standalone pass skips; a merged one re-records") {
        RtSlot s;
        s.generation = 3;
        auto pass    = cachedPass(&s, 3);
        auto d       = pass->cacheDecision(PassMergeRole::Standalone, false, true);
        CHECK(d.skip);
        CHECK_FALSE(d.invalidate);
        auto merged = pass->cacheDecision(PassMergeRole::GroupBegin, false, true);
        CHECK_FALSE(merged.skip);
        CHECK_FALSE(merged.invalidate);
    }
    TEST_CASE("a new image in the output slot drops the cache") {
        RtSlot s;
        s.generation = 3;
        auto pass    = cachedPass(&s, 3);
        ++s.generation;
        auto d = pass->cacheDecision(PassMergeRole::Standalone, false, true);
        CHECK(d.invalidate);
        CHECK_FALSE(d.skip);
    }
    TEST_CASE("a pass without an output slot never sees a generation change") {
        auto pass = cachedPass(nullptr, 0);
        auto d    = pass->cacheDecision(PassMergeRole::Standalone, false, true);
        CHECK(d.skip);
        CHECK_FALSE(d.invalidate);
    }
}

TEST_SUITE("CustomShaderPass::outputSkipsRecord") {
    using wallpaper::vulkan::RtSlot;

    TEST_CASE("a managed output with no image skips once nobody needs it") {
        RtSlot                 s;
        CustomShaderPass::Desc d;
        d.out_slot = &s;
        CustomShaderPass pass(d);
        pass.setOutputNeeded(false);
        CHECK(pass.outputSkipsRecord());
        s.image.emplace();
        CHECK(pass.outputSkipsRecord()); // resident but unneeded
        pass.setOutputNeeded(true);
        CHECK_FALSE(pass.outputSkipsRecord());
    }
    TEST_CASE("a persistent output never skips") {
        RtSlot s;
        s.persist = true;
        CustomShaderPass::Desc d;
        d.out_slot = &s;
        CustomShaderPass pass(d);
        pass.setOutputNeeded(false);
        CHECK_FALSE(pass.outputSkipsRecord());
    }
    TEST_CASE("a pass without an output slot never skips") {
        CustomShaderPass::Desc d;
        CustomShaderPass       pass(d);
        pass.setOutputNeeded(false);
        CHECK_FALSE(pass.outputSkipsRecord());
    }
}

TEST_SUITE("CustomShaderPass::PassthroughSamplerIndex") {
    using wallpaper::vulkan::PassthroughSamplerIndex;

    TEST_CASE("binding 0 and texture 0 are valid") {
        CHECK(PassthroughSamplerIndex(0, 0, 1) == std::optional<std::size_t> { 0 });
        CHECK(PassthroughSamplerIndex(1, 0, 1) == std::optional<std::size_t> { 0 });
    }
    TEST_CASE("an in-range index is returned") {
        CHECK(PassthroughSamplerIndex(1, 2, 3) == std::optional<std::size_t> { 2 });
    }
    TEST_CASE("a missing sampler binding or index gives nothing") {
        CHECK_FALSE(PassthroughSamplerIndex(-1, 2, 3).has_value());
        CHECK_FALSE(PassthroughSamplerIndex(1, -1, 3).has_value());
    }
    TEST_CASE("an index at or past the texture count gives nothing") {
        CHECK_FALSE(PassthroughSamplerIndex(1, 3, 3).has_value());
        CHECK_FALSE(PassthroughSamplerIndex(1, 5, 3).has_value());
    }
}

TEST_SUITE("CustomShaderPass::SlotAt") {
    using wallpaper::vulkan::RtSlot;
    using wallpaper::vulkan::SlotAt;

    TEST_CASE("an index inside the list returns that slot") {
        RtSlot               a, b;
        std::vector<RtSlot*> v { &a, &b };
        CHECK(SlotAt(v, 0) == &a);
        CHECK(SlotAt(v, 1) == &b);
    }
    TEST_CASE("an index at or past the end returns null") {
        RtSlot               a, b;
        std::vector<RtSlot*> v { &a, &b };
        v.pop_back(); // capacity still holds &b
        CHECK(SlotAt(v, 1) == nullptr);
        CHECK(SlotAt(v, 7) == nullptr);
    }
}

TEST_SUITE("CustomShaderPass::PlanTextureBinds") {
    using wallpaper::i32;
    using wallpaper::vulkan::ImageSlotsRef;
    using wallpaper::vulkan::PlanTextureBind;
    using wallpaper::vulkan::PlanTextureBindsInto;
    using wallpaper::vulkan::RtSlot;
    using wallpaper::vulkan::TexBind;
    using wallpaper::vulkan::TexBindStep;

    // A texture list where `loaded[i]` says whether texture i has an image.
    static std::vector<ImageSlotsRef> textures(std::vector<bool> loaded) {
        std::vector<ImageSlotsRef> t(loaded.size());
        for (std::size_t i = 0; i < loaded.size(); i++)
            if (loaded[i]) t[i].slots.emplace_back();
        return t;
    }
    static std::vector<TexBind> how(const std::vector<TexBindStep>& plan) {
        std::vector<TexBind> h;
        for (auto& s : plan) h.push_back(s.how);
        return h;
    }

    TEST_CASE("each texture is skipped, imaged or backed by the fallback") {
        std::vector<TexBindStep> plan;
        PlanTextureBindsInto(plan, textures({ true, true, false }), { 1, -1, 2 }, {}, true, true);
        REQUIRE(plan.size() == 3);
        CHECK(how(plan) ==
              std::vector<TexBind> { TexBind::Image, TexBind::Skip, TexBind::Fallback });
        CHECK(plan[0].index == 0);
        CHECK(plan[2].index == 2);
    }
    TEST_CASE("every texture is planned, not just the first") {
        std::vector<TexBindStep> plan;
        PlanTextureBindsInto(plan, textures({ true, true }), { 1, 2 }, {}, true, true);
        REQUIRE(plan.size() == 2);
        CHECK(plan[1].how == TexBind::Image);
    }
    TEST_CASE("binding 0 is a declared binding, only negative ones are skipped") {
        std::vector<TexBindStep> plan;
        PlanTextureBindsInto(plan, textures({ true }), { 0 }, {}, true, true);
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].how == TexBind::Image);
        CHECK(PlanTextureBind(-1, true, nullptr, true, true) == TexBind::Skip);
    }
    TEST_CASE("a record that does not draw omits an absent managed input") {
        RtSlot absent;
        CHECK(PlanTextureBind(1, false, &absent, false, true) == TexBind::Omit);
        CHECK(PlanTextureBind(1, false, &absent, true, true) == TexBind::Fallback);
        CHECK(PlanTextureBind(1, false, nullptr, false, true) == TexBind::Fallback);
    }
    TEST_CASE("without a fallback texture an absent input is omitted") {
        CHECK(PlanTextureBind(1, false, nullptr, true, false) == TexBind::Omit);
    }
    TEST_CASE("an absent managed input found through in_slots is seen") {
        RtSlot                   absent;
        std::vector<RtSlot*>     in { &absent };
        std::vector<TexBindStep> plan;
        PlanTextureBindsInto(plan, textures({ false }), { 1 }, in, false, true);
        REQUIRE(plan.size() == 1);
        CHECK(plan[0].how ==
              TexBind::Omit); // without the in_slots lookup this would bind the fallback
    }
    TEST_CASE("a reused plan is rebuilt, not appended to") {
        std::vector<TexBindStep> plan;
        PlanTextureBindsInto(plan, textures({ true, true }), { 1, 2 }, {}, true, true);
        PlanTextureBindsInto(plan, textures({ true }), { 1 }, {}, true, true);
        CHECK(plan.size() == 1);
    }
}

TEST_SUITE("CopyPass skip decision") {
    using wallpaper::vulkan::CopySkips;
    using wallpaper::vulkan::RtSlot;

    static void resident(RtSlot & s) { s.image.emplace(); }

    TEST_CASE("a resident source and destination copy") {
        RtSlot s, d;
        resident(s);
        resident(d);
        CHECK_FALSE(CopySkips(&s, &d, true));
    }
    TEST_CASE("a missing side skips") {
        RtSlot s, d;
        resident(s);
        resident(d);
        CHECK(CopySkips(nullptr, &d, true));
        CHECK(CopySkips(&s, nullptr, true));
    }
    TEST_CASE("a source with no image skips") {
        RtSlot s, d;
        resident(d);
        CHECK(CopySkips(&s, &d, true));
    }
    TEST_CASE("a managed destination skips when it has no image or nobody needs it") {
        RtSlot s, d;
        resident(s);
        CHECK(CopySkips(&s, &d, true)); // no image
        resident(d);
        CHECK(CopySkips(&s, &d, false)); // resident but unneeded
    }
    TEST_CASE("a persistent destination is never gated") {
        RtSlot s, d;
        resident(s);
        d.persist = true;
        CHECK_FALSE(CopySkips(&s, &d, false));
    }
}

TEST_SUITE("SkyboxPass output slot") {
    using wallpaper::vulkan::RtSlot;
    using wallpaper::vulkan::SlotHoldsFixedImage;

    TEST_CASE("no slot or a persistent slot holds a fixed image; a managed one does not") {
        CHECK(SlotHoldsFixedImage(nullptr));
        RtSlot p;
        p.persist = true;
        CHECK(SlotHoldsFixedImage(&p));
        RtSlot m;
        m.persist = false;
        CHECK_FALSE(SlotHoldsFixedImage(&m));
    }
}
