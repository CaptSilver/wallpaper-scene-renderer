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

        auto owner = std::make_shared<VmaImageParameters>();
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
        auto first = std::make_shared<VmaImageParameters>();
        first->initial_layout_transitioned = true;  // simulate a first-use barrier emitted

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
