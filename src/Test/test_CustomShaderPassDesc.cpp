#include <doctest.h>

#include "VulkanRender/CustomShaderPass.hpp"
#include "Vulkan/Parameters.hpp"

using wallpaper::vulkan::CustomShaderPass;
using wallpaper::vulkan::VmaImageParameters;

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
