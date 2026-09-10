#include <doctest.h>

#include <vulkan/vulkan.h>

#include <vector>

#include "Vulkan/TextureCache.hpp"
#include "vvk/vulkan_wrapper.hpp"

using wallpaper::TextureFormat;
using wallpaper::vulkan::GenInitialClearColor;
using wallpaper::vulkan::RecClearNewImage;
using wallpaper::vulkan::TextureKey;

namespace
{

struct Recording {
    int                               clear_calls { 0 };
    VkImageLayout                     clear_layout { VK_IMAGE_LAYOUT_UNDEFINED };
    VkClearColorValue                 color {};
    std::vector<VkImageMemoryBarrier> barriers;
};

Recording g_rec;

void VKAPI_PTR recordClear(VkCommandBuffer, VkImage, VkImageLayout layout,
                           const VkClearColorValue* color, uint32_t,
                           const VkImageSubresourceRange*) {
    g_rec.clear_calls++;
    g_rec.clear_layout = layout;
    g_rec.color        = *color;
}

void VKAPI_PTR recordBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                             VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t,
                             const VkBufferMemoryBarrier*, uint32_t count,
                             const VkImageMemoryBarrier* barriers) {
    for (uint32_t i = 0; i < count; i++) g_rec.barriers.push_back(barriers[i]);
}

// Drives the recording path through a dispatch table of local functions, so the
// commands can be inspected without a Vulkan device.
Recording record(const VkClearColorValue& color, VkImageLayout final_layout) {
    g_rec = Recording {};

    vvk::DeviceDispatch dld {};
    dld.vkCmdClearColorImage = recordClear;
    dld.vkCmdPipelineBarrier = recordBarrier;

    vvk::CommandBuffer cmd(reinterpret_cast<VkCommandBuffer>(0x1), dld);
    RecClearNewImage(cmd, reinterpret_cast<VkImage>(0x2), color, final_layout);
    return g_rec;
}

TextureKey keyOf(int width, int height) {
    return TextureKey {
        .width        = width,
        .height       = height,
        .usage        = {},
        .format       = TextureFormat::RGBA8,
        .sample       = {},
        .mipmap_level = 1,
    };
}

} // namespace

// A cache image comes out of the allocator holding whatever the last user of
// that memory left in it, so anything sampling it before a pass writes it —
// the placeholder bound to shader slots with no texture, above all — reads
// different garbage on every run.  Give it contents.
TEST_SUITE("TextureCache initial contents") {
    TEST_CASE("a new image is cleared before it can be sampled") {
        const VkClearColorValue color { { 0.25f, 0.5f, 0.75f, 1.0f } };
        const auto              rec = record(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        REQUIRE(rec.clear_calls == 1);
        CHECK(rec.color.float32[0] == doctest::Approx(0.25f));
        CHECK(rec.color.float32[1] == doctest::Approx(0.5f));
        CHECK(rec.color.float32[2] == doctest::Approx(0.75f));
        CHECK(rec.color.float32[3] == doctest::Approx(1.0f));
    }

    TEST_CASE("the clear runs in transfer-dst layout and ends in the requested one") {
        const VkClearColorValue color { { 0.0f, 0.0f, 0.0f, 0.0f } };
        const auto              rec = record(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        CHECK(rec.clear_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        REQUIRE(rec.barriers.size() == 2);
        CHECK(rec.barriers.front().oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        CHECK(rec.barriers.front().newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        CHECK(rec.barriers.back().newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    TEST_CASE("the 1x1 placeholder gets a normal-map-neutral colour") {
        const auto color = GenInitialClearColor(keyOf(1, 1));

        CHECK(color.float32[0] == doctest::Approx(0.5f));
        CHECK(color.float32[1] == doctest::Approx(0.5f));
        CHECK(color.float32[2] == doctest::Approx(1.0f));
        CHECK(color.float32[3] == doctest::Approx(1.0f));
    }

    TEST_CASE("render targets start transparent black") {
        const auto color = GenInitialClearColor(keyOf(1920, 1080));

        CHECK(color.float32[0] == doctest::Approx(0.0f));
        CHECK(color.float32[1] == doctest::Approx(0.0f));
        CHECK(color.float32[2] == doctest::Approx(0.0f));
        CHECK(color.float32[3] == doctest::Approx(0.0f));
    }
}
