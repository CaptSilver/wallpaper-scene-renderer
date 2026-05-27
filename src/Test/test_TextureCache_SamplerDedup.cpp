#include <doctest.h>

#include <vulkan/vulkan.h>

#include "Vulkan/SamplerDedupDetail.hpp"

using namespace wallpaper::vulkan;

// Cache the VkSampler used by TextureCache::CreateTex.  Pre-dedup, every
// texture was given its own VkSampler even when the sampler config
// (filter/address-mode/anisotropy/etc.) matched one already created — wasted
// driver-side sampler allocations and a small amount of GPU register pressure.
//
// The behaviour we pin here: two textures with the SAME sampler config share
// the same VkSampler handle; two textures with DIFFERENT configs get
// independent handles.  The cache is driven through a creator-callback so the
// test can verify dedup without a live Vulkan device.
TEST_SUITE("TextureCache VkSampler dedup") {
    static VkSamplerCreateInfo baselineInfo() {
        VkSamplerCreateInfo info {};
        info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter               = VK_FILTER_LINEAR;
        info.minFilter               = VK_FILTER_LINEAR;
        info.mipmapMode              = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.addressModeU            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW            = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.anisotropyEnable        = VK_FALSE;
        info.maxAnisotropy           = 1.0f;
        info.compareEnable           = VK_FALSE;
        info.compareOp               = VK_COMPARE_OP_NEVER;
        info.minLod                  = 0.0f;
        info.maxLod                  = 12.0f;
        info.borderColor             = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;
        return info;
    }

    // Tiny test-only handle stand-in.  The real cache stores vvk::Sampler, but
    // here we exercise the generic findOrCreate seam with a uint64_t so the
    // contract is testable without a Vulkan device.
    struct FakeSampler {
        uint64_t value { 0 };
    };

    TEST_CASE("two textures with identical sampler config share one handle") {
        SamplerBucketMap<FakeSampler> cache;
        int                           create_calls = 0;
        uint64_t                      next_handle  = 100;
        auto                          creator = [&](const VkSamplerCreateInfo&) -> FakeSampler {
            ++create_calls;
            return FakeSampler { next_handle++ };
        };

        auto           a       = baselineInfo();
        const uint64_t a_value = findOrCreateSampler(cache, a, creator).value;
        // Second texture with the same config: should hit the cache.
        auto           b       = baselineInfo();
        const uint64_t b_value = findOrCreateSampler(cache, b, creator).value;

        CHECK(create_calls == 1);
        CHECK(a_value == b_value);
    }

    TEST_CASE("two textures with different sampler config get independent handles") {
        SamplerBucketMap<FakeSampler> cache;
        int                           create_calls = 0;
        uint64_t                      next_handle  = 100;
        auto                          creator = [&](const VkSamplerCreateInfo&) -> FakeSampler {
            ++create_calls;
            return FakeSampler { next_handle++ };
        };

        auto a      = baselineInfo();
        auto b      = baselineInfo();
        b.magFilter = VK_FILTER_NEAREST; // differs

        const uint64_t a_value = findOrCreateSampler(cache, a, creator).value;
        const uint64_t b_value = findOrCreateSampler(cache, b, creator).value;

        CHECK(create_calls == 2);
        CHECK(a_value != b_value);
    }

    TEST_CASE("hashSamplerInfo: identical configs hash identical") {
        auto a = baselineInfo();
        auto b = baselineInfo();
        CHECK(hashSamplerInfo(a) == hashSamplerInfo(b));
    }

    TEST_CASE("hashSamplerInfo: any meaningful field difference changes hash") {
        auto a = baselineInfo();

        auto m      = baselineInfo();
        m.magFilter = VK_FILTER_NEAREST;
        CHECK(hashSamplerInfo(a) != hashSamplerInfo(m));

        auto u         = baselineInfo();
        u.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        CHECK(hashSamplerInfo(a) != hashSamplerInfo(u));

        auto l   = baselineInfo();
        l.maxLod = 1.0f;
        CHECK(hashSamplerInfo(a) != hashSamplerInfo(l));

        auto an             = baselineInfo();
        an.anisotropyEnable = VK_TRUE;
        an.maxAnisotropy    = 16.0f;
        CHECK(hashSamplerInfo(a) != hashSamplerInfo(an));
    }

    TEST_CASE("samplerInfoEqual: reflexive on identical, sensitive on any field") {
        auto a = baselineInfo();
        auto b = baselineInfo();
        CHECK(samplerInfoEqual(a, b));

        auto m      = baselineInfo();
        m.magFilter = VK_FILTER_NEAREST;
        CHECK(! samplerInfoEqual(a, m));

        auto bc        = baselineInfo();
        bc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        CHECK(! samplerInfoEqual(a, bc));
    }

    TEST_CASE("collision robustness: bucket scan disambiguates on equality") {
        // Force the same hash bucket via an injected hash that maps every
        // input to 0 — exercises the inner samplerInfoEqual scan that the
        // real cache uses when two configs accidentally collide.
        SamplerBucketMap<FakeSampler> cache;
        int                           create_calls = 0;
        uint64_t                      next_handle  = 200;
        auto                          creator = [&](const VkSamplerCreateInfo&) -> FakeSampler {
            ++create_calls;
            return FakeSampler { next_handle++ };
        };
        auto force_collision_hash = [](const VkSamplerCreateInfo&) -> std::size_t {
            return 0;
        };

        auto a      = baselineInfo();
        auto b      = baselineInfo();
        b.magFilter = VK_FILTER_NEAREST;

        // Record the value at first miss; we do NOT keep the reference across
        // a subsequent emplace_back (which can reallocate the bucket vector).
        const uint64_t a_value =
            findOrCreateSamplerWithHash(cache, a, creator, force_collision_hash).value;
        const uint64_t b_value =
            findOrCreateSamplerWithHash(cache, b, creator, force_collision_hash).value;

        CHECK(create_calls == 2);
        CHECK(a_value != b_value);

        // Re-request a; the collision scan must find the existing entry rather
        // than treating it as a new one in bucket 0.
        const uint64_t a_again =
            findOrCreateSamplerWithHash(cache, a, creator, force_collision_hash).value;
        CHECK(create_calls == 2);
        CHECK(a_again == a_value);
    }
}
