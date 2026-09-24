#include <doctest.h>

#include "VulkanRender/UniformWriteSize.hpp"

using wallpaper::vulkan::ClampUniformWriteSize;
using wallpaper::vulkan::ResolveUniformMemberWriteSize;

TEST_SUITE("ResolveUniformMemberWriteSize") {
    TEST_CASE("a reflected size passes through unchanged") {
        CHECK(ResolveUniformMemberWriteSize(64).value() == 64u); // e.g. a mat4 member
    }

    TEST_CASE("zero reflected size has no safe write size to fall back to") {
        CHECK_FALSE(ResolveUniformMemberWriteSize(0).has_value());
    }
}

TEST_SUITE("ClampUniformWriteSize") {
    TEST_CASE("a value smaller than the member is written whole") {
        CHECK(ClampUniformWriteSize(48, 64) == 48u); // vec3 into a mat4-sized member
    }

    TEST_CASE("a value the same size as the member is written whole") {
        CHECK(ClampUniformWriteSize(64, 64) == 64u);
    }

    TEST_CASE("a value wider than the member is truncated to the member's size") {
        // mat4 (64 bytes) written into a mat3 (36 bytes) slot must not spill
        // into whatever UBO member sits right after it.
        CHECK(ClampUniformWriteSize(64, 36) == 36u);
    }
}
