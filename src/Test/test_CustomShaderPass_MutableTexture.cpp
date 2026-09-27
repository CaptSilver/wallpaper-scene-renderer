#include <doctest.h>

#include "VulkanRender/CustomShaderPass.hpp"

using wallpaper::vulkan::ReferencesMediaSystemTexture;

// prepare() needs a live Vulkan Device to run end to end, so it can't be
// exercised here (see the file-level comment in CustomShaderPass.cpp). What
// can be pinned down without one is the piece of its per-texture loop that
// decides whether a pass touches a "Now Playing" system texture at all --
// prepare() ORs this into m_samples_mutable_texture once per Desc, right
// where EnsureSceneDepthTextureIfNeeded runs. Get this wrong and a pass
// sampling $mediaThumbnail/$mediaPreviousThumbnail gets marked cacheable,
// stops re-executing after frame 1, and the reupload a track change sends
// never reaches the screen.
TEST_SUITE("CustomShaderPass::media-system-texture") {
    TEST_CASE("a pass sampling $mediaThumbnail is flagged") {
        CHECK(ReferencesMediaSystemTexture({ "SS", "$mediaThumbnail" }));
    }
    TEST_CASE("a pass sampling $mediaPreviousThumbnail is flagged") {
        CHECK(ReferencesMediaSystemTexture({ "$mediaPreviousThumbnail" }));
    }
    TEST_CASE("an ordinary texture list is not flagged") {
        CHECK_FALSE(ReferencesMediaSystemTexture({ "SS", "diffuse", "" }));
    }
    TEST_CASE("an empty texture list is not flagged") {
        CHECK_FALSE(ReferencesMediaSystemTexture({}));
    }
}
