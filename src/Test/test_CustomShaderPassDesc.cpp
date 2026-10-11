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

// ShouldDrawPassthrough gates the pipeline-select branch in execute(): does
// THIS frame draw the terminal's passthrough pipeline instead of its real
// one?  Only when the pass both carries a runtime visibility binding on the
// terminal position (is_terminal_hideable) AND the node is hidden right now
// AND prepare() actually managed to build the passthrough pipeline and UBO.
TEST_SUITE("CustomShaderPass::ShouldDrawPassthrough") {
    using wallpaper::vulkan::ShouldDrawPassthrough;

    TEST_CASE("an ordinary (non-terminal-hideable) pass never draws passthrough, hidden or not") {
        CHECK_FALSE(ShouldDrawPassthrough(
            /*is_terminal_hideable=*/false, /*node_hidden=*/false, /*passthrough_ready=*/false));
        // Can't happen through the render graph today (nothing marks a
        // non-hideable pass this way), but pins the byte-identical
        // blast-radius contract: is_terminal_hideable is the only thing that
        // can turn this on.
        CHECK_FALSE(ShouldDrawPassthrough(
            /*is_terminal_hideable=*/false, /*node_hidden=*/true, /*passthrough_ready=*/true));
    }
    TEST_CASE("a terminal-hideable pass that's currently visible draws its real material") {
        CHECK_FALSE(ShouldDrawPassthrough(
            /*is_terminal_hideable=*/true, /*node_hidden=*/false, /*passthrough_ready=*/true));
    }
    TEST_CASE("a terminal-hideable pass that's hidden draws the passthrough") {
        CHECK(ShouldDrawPassthrough(
            /*is_terminal_hideable=*/true, /*node_hidden=*/true, /*passthrough_ready=*/true));
    }
    TEST_CASE("a hidden terminal-hideable pass whose passthrough failed to build never binds it") {
        // prepare() leaves passthrough_pipeline default-constructed when the
        // shader compile, reflection, render pass, pipeline or UBO step fails;
        // binding that null handle would be invalid Vulkan usage.
        CHECK_FALSE(ShouldDrawPassthrough(
            /*is_terminal_hideable=*/true, /*node_hidden=*/true, /*passthrough_ready=*/false));
    }
}

// HiddenTerminalKeepsRealDraw is the fallback for the last case above: with no
// passthrough to draw, a hidden terminal-hideable pass keeps drawing its real
// material -- the same thing it did before terminal effects could be hidden at
// all -- rather than dropping its draw and blanking the whole layer.
TEST_SUITE("CustomShaderPass::HiddenTerminalKeepsRealDraw") {
    using wallpaper::vulkan::HiddenTerminalKeepsRealDraw;

    TEST_CASE("only a hidden terminal-hideable pass with no passthrough falls back to its real "
              "draw") {
        CHECK(HiddenTerminalKeepsRealDraw(
            /*is_terminal_hideable=*/true, /*node_hidden=*/true, /*passthrough_ready=*/false));
    }
    TEST_CASE("a working passthrough, a visible node, or an ordinary pass never takes the "
              "fallback") {
        CHECK_FALSE(HiddenTerminalKeepsRealDraw(
            /*is_terminal_hideable=*/true, /*node_hidden=*/true, /*passthrough_ready=*/true));
        CHECK_FALSE(HiddenTerminalKeepsRealDraw(
            /*is_terminal_hideable=*/true, /*node_hidden=*/false, /*passthrough_ready=*/false));
        // An ordinary hidden pass keeps the existing skip-or-draw-nothing path.
        CHECK_FALSE(HiddenTerminalKeepsRealDraw(
            /*is_terminal_hideable=*/false, /*node_hidden=*/true, /*passthrough_ready=*/false));
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
    TEST_CASE("is_terminal_hideable defaults to false") {
        CustomShaderPass::Desc desc;
        CHECK_FALSE(desc.is_terminal_hideable);
    }
    TEST_CASE("passthrough_input_tex_index_a and _b default to -1") {
        CustomShaderPass::Desc desc;
        CHECK(desc.passthrough_input_tex_index_a == -1);
        CHECK(desc.passthrough_input_tex_index_b == -1);
    }
    TEST_CASE("passthrough_predecessor_nodes defaults empty") {
        CustomShaderPass::Desc desc;
        CHECK(desc.passthrough_predecessor_nodes.empty());
    }
    TEST_CASE("passthrough_tex_binding defaults to -1") {
        CustomShaderPass::Desc desc;
        CHECK(desc.passthrough_tex_binding == -1);
    }
}

// IsHiddenPassSkippable alone would let a hidden terminal-hideable pass on a
// non-clearing RT (the shared _rt_default, whenever this object isn't the
// first thing drawn onto the composite) take the early-return skip and never
// record anything -- silently breaking the "hiding it still draws something"
// contract the passthrough pipeline depends on.  The guard at
// CustomShaderPass::execute()'s invisible-node fast path adds the
// is_terminal_hideable exemption; this wrapper mirrors that exact
// conjunction so the exemption is covered without a live Device.
namespace
{
bool wouldTakeHiddenSkipPath(bool is_terminal_hideable, bool node_hidden, bool clears_output) {
    return ! is_terminal_hideable &&
           wallpaper::vulkan::IsHiddenPassSkippable(node_hidden, clears_output);
}
} // namespace

TEST_SUITE("CustomShaderPass terminal-hideable skip-guard exemption") {
    TEST_CASE("an ordinary hidden pass on a non-clearing RT takes the skip path") {
        CHECK(wouldTakeHiddenSkipPath(
            /*is_terminal_hideable=*/false, /*node_hidden=*/true, /*clears_output=*/false));
    }
    TEST_CASE("a hidden terminal-hideable pass on the SAME shape does NOT take the skip path") {
        // clears_output=false is exactly the shared _rt_default, not-first-
        // layer case -- without the exemption this is the one that silently
        // never records.
        CHECK_FALSE(wouldTakeHiddenSkipPath(
            /*is_terminal_hideable=*/true, /*node_hidden=*/true, /*clears_output=*/false));
    }
    TEST_CASE("a hidden terminal-hideable pass that owns its RT's clear still never skips either "
              "way") {
        // clears_output=true already made IsHiddenPassSkippable false on its
        // own (the private-offscreen-RT case) -- the exemption changes
        // nothing there, it's only load-bearing for clears_output=false.
        CHECK_FALSE(wouldTakeHiddenSkipPath(
            /*is_terminal_hideable=*/true, /*node_hidden=*/true, /*clears_output=*/true));
        CHECK_FALSE(wouldTakeHiddenSkipPath(
            /*is_terminal_hideable=*/false, /*node_hidden=*/true, /*clears_output=*/true));
    }
}

// A terminal-hideable pass appends 2 extra texture reads (the chain's two
// ping-pong buffers) on top of however many the real material already
// declares, so an index built from vk_textures.size() can run past
// WE_GLTEX_NAMES' fixed 13 entries -- Wallpaper Engine materials only ever
// declare g_Texture0 through g_Texture12. GLTexNameForIndex is the
// bounds-checked stand-in for indexing that array directly.
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
