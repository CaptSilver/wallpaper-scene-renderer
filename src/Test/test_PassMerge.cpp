#include <doctest.h>

#include "VulkanRender/PassMerge.hpp"

#include <cstdint>
#include <vector>

using namespace wallpaper::vulkan;

// Pure decision layer behind recording several consecutive scene passes
// inside ONE Vulkan render pass instance instead of one begin/end cycle
// each.  Handles are plain integers here, so no Vulkan device is needed.
namespace
{
using H                = std::uintptr_t;
constexpr H RT_VIEW    = 0x1001; // colour / resolve view of _rt_default
constexpr H RT_IMAGE   = 0x1002;
constexpr H MSAA_VIEW  = 0x1003;
constexpr H MSAA_IMAGE = 0x1004;
constexpr H DEPTH_VIEW = 0x1005;
constexpr H DEPTH_IMG  = 0x1006;
constexpr H OTHER_VIEW = 0x2001;
constexpr H OTHER_IMG  = 0x2002;
constexpr H A_TEXTURE  = 0x3001;

// A plain pass writing _rt_default with 4x MSAA and a depth attachment,
// sampling one ordinary texture.
PassMergeInfo<H> DefaultRtPass() {
    PassMergeInfo<H> p;
    p.mergeable         = true;
    p.color_view        = RT_VIEW;
    p.msaa_view         = MSAA_VIEW;
    p.depth_view        = DEPTH_VIEW;
    p.samples           = 4;
    p.width             = 3840;
    p.height            = 2160;
    p.clears_output     = false;
    p.attachment_images = { RT_IMAGE, MSAA_IMAGE, DEPTH_IMG };
    p.sampled_images    = { A_TEXTURE };
    return p;
}

std::vector<PassMergeRole> Roles(const std::vector<PassMergeInfo<H>>& v) {
    return ComputePassMergeRoles<H>(v);
}
} // namespace

TEST_SUITE("PassMerge::ComputePassMergeRoles") {
    TEST_CASE("no passes gives no roles") { CHECK(Roles({}).empty()); }

    TEST_CASE("a lone pass stays standalone") {
        auto r = Roles({ DefaultRtPass() });
        REQUIRE(r.size() == 1);
        CHECK(r[0] == PassMergeRole::Standalone);
    }

    TEST_CASE("two consecutive writers of the same target become one group") {
        auto first          = DefaultRtPass();
        first.clears_output = true; // first writer of the frame owns the clear
        auto r              = Roles({ first, DefaultRtPass() });
        REQUIRE(r.size() == 2);
        CHECK(r[0] == PassMergeRole::GroupBegin);
        CHECK(r[1] == PassMergeRole::GroupEnd);
    }

    TEST_CASE("a long run gets one begin, one end and middles between") {
        std::vector<PassMergeInfo<H>> v(5, DefaultRtPass());
        auto                          r = Roles(v);
        REQUIRE(r.size() == 5);
        CHECK(r[0] == PassMergeRole::GroupBegin);
        CHECK(r[1] == PassMergeRole::GroupMiddle);
        CHECK(r[2] == PassMergeRole::GroupMiddle);
        CHECK(r[3] == PassMergeRole::GroupMiddle);
        CHECK(r[4] == PassMergeRole::GroupEnd);
    }

    TEST_CASE("a different output target splits the run") {
        auto other              = DefaultRtPass();
        other.color_view        = OTHER_VIEW;
        other.msaa_view         = H {};
        other.samples           = 1;
        other.attachment_images = { OTHER_IMG, DEPTH_IMG };

        auto r = Roles({ DefaultRtPass(), other, DefaultRtPass(), DefaultRtPass() });
        REQUIRE(r.size() == 4);
        CHECK(r[0] == PassMergeRole::Standalone);
        CHECK(r[1] == PassMergeRole::Standalone);
        CHECK(r[2] == PassMergeRole::GroupBegin);
        CHECK(r[3] == PassMergeRole::GroupEnd);
    }

    TEST_CASE("a second clear inside the run starts a new group") {
        // Only the pass that opens a render pass gets its load op honoured,
        // so a later writer that must CLEAR cannot be folded into the group.
        auto reclear          = DefaultRtPass();
        reclear.clears_output = true;

        auto r = Roles({ DefaultRtPass(), reclear, DefaultRtPass() });
        REQUIRE(r.size() == 3);
        CHECK(r[0] == PassMergeRole::Standalone);
        CHECK(r[1] == PassMergeRole::GroupBegin);
        CHECK(r[2] == PassMergeRole::GroupEnd);
    }

    TEST_CASE("a pass that samples the target it writes is never merged") {
        // Reading the attachment needs the render pass boundary as its
        // barrier: an effect chain sampling _rt_default, a copy, a bloom
        // stage.  Merging would drop the synchronisation the read depends on.
        auto reader           = DefaultRtPass();
        reader.sampled_images = { RT_IMAGE };

        auto r = Roles({ DefaultRtPass(), reader, DefaultRtPass(), DefaultRtPass() });
        REQUIRE(r.size() == 4);
        CHECK(r[0] == PassMergeRole::Standalone);
        CHECK(r[1] == PassMergeRole::Standalone);
        CHECK(r[2] == PassMergeRole::GroupBegin);
        CHECK(r[3] == PassMergeRole::GroupEnd);
    }

    TEST_CASE("sampling the group's depth or MSAA image also blocks the merge") {
        auto depth_reader           = DefaultRtPass();
        depth_reader.sampled_images = { DEPTH_IMG };
        auto rd                     = Roles({ DefaultRtPass(), depth_reader });
        REQUIRE(rd.size() == 2);
        CHECK(rd[0] == PassMergeRole::Standalone);
        CHECK(rd[1] == PassMergeRole::Standalone);

        auto msaa_reader           = DefaultRtPass();
        msaa_reader.sampled_images = { MSAA_IMAGE };
        auto rm                    = Roles({ DefaultRtPass(), msaa_reader });
        REQUIRE(rm.size() == 2);
        CHECK(rm[0] == PassMergeRole::Standalone);
        CHECK(rm[1] == PassMergeRole::Standalone);
    }

    TEST_CASE("a mismatched attachment configuration blocks the merge") {
        SUBCASE("sample count") {
            auto b    = DefaultRtPass();
            b.samples = 1;
            auto r    = Roles({ DefaultRtPass(), b });
            CHECK(r[0] == PassMergeRole::Standalone);
            CHECK(r[1] == PassMergeRole::Standalone);
        }
        SUBCASE("depth attachment present on one side only") {
            auto b       = DefaultRtPass();
            b.depth_view = H {};
            auto r       = Roles({ DefaultRtPass(), b });
            CHECK(r[0] == PassMergeRole::Standalone);
            CHECK(r[1] == PassMergeRole::Standalone);
        }
        SUBCASE("resolve target") {
            auto b      = DefaultRtPass();
            b.msaa_view = 0x9999;
            auto r      = Roles({ DefaultRtPass(), b });
            CHECK(r[0] == PassMergeRole::Standalone);
            CHECK(r[1] == PassMergeRole::Standalone);
        }
        SUBCASE("render area") {
            auto b   = DefaultRtPass();
            b.width  = 1920;
            b.height = 1080;
            auto r   = Roles({ DefaultRtPass(), b });
            CHECK(r[0] == PassMergeRole::Standalone);
            CHECK(r[1] == PassMergeRole::Standalone);
        }
    }

    TEST_CASE("a pass that opted out of merging breaks the run around it") {
        // PrePass / FinPass / CopyPass and any pass that can skip itself on
        // re-execute arrive with mergeable=false.
        auto opaque      = DefaultRtPass();
        opaque.mergeable = false;

        auto r =
            Roles({ DefaultRtPass(), DefaultRtPass(), opaque, DefaultRtPass(), DefaultRtPass() });
        REQUIRE(r.size() == 5);
        CHECK(r[0] == PassMergeRole::GroupBegin);
        CHECK(r[1] == PassMergeRole::GroupEnd);
        CHECK(r[2] == PassMergeRole::Standalone);
        CHECK(r[3] == PassMergeRole::GroupBegin);
        CHECK(r[4] == PassMergeRole::GroupEnd);
    }

    TEST_CASE("a pass with no colour attachment is never merged") {
        auto novi       = DefaultRtPass();
        novi.color_view = H {};
        auto r          = Roles({ novi, novi });
        CHECK(r[0] == PassMergeRole::Standalone);
        CHECK(r[1] == PassMergeRole::Standalone);
    }
}

TEST_SUITE("PassMerge role predicates") {
    TEST_CASE("only the standalone and opening roles begin a render pass") {
        CHECK(PassOpensRenderPass(PassMergeRole::Standalone));
        CHECK(PassOpensRenderPass(PassMergeRole::GroupBegin));
        CHECK_FALSE(PassOpensRenderPass(PassMergeRole::GroupMiddle));
        CHECK_FALSE(PassOpensRenderPass(PassMergeRole::GroupEnd));
    }

    TEST_CASE("only the standalone and closing roles end a render pass") {
        CHECK(PassClosesRenderPass(PassMergeRole::Standalone));
        CHECK(PassClosesRenderPass(PassMergeRole::GroupEnd));
        CHECK_FALSE(PassClosesRenderPass(PassMergeRole::GroupBegin));
        CHECK_FALSE(PassClosesRenderPass(PassMergeRole::GroupMiddle));
    }

    TEST_CASE("a hidden pass only drops its whole recording when it owes no begin or end") {
        // Vulkan has no way to close a render pass someone else opened, so a
        // hidden pass holding the group's begin or end still records the
        // boundary — it just contributes no draw.
        CHECK(HiddenPassDropsRecording(PassMergeRole::Standalone));
        CHECK(HiddenPassDropsRecording(PassMergeRole::GroupMiddle));
        CHECK_FALSE(HiddenPassDropsRecording(PassMergeRole::GroupBegin));
        CHECK_FALSE(HiddenPassDropsRecording(PassMergeRole::GroupEnd));
    }
}
