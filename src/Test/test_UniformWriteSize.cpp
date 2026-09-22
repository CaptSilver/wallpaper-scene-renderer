#include <doctest.h>

#include "VulkanRender/UniformWriteSize.hpp"

using wallpaper::vulkan::ResolveUniformMemberWriteSize;

TEST_SUITE("ResolveUniformMemberWriteSize") {
    TEST_CASE("a reflected size passes through unchanged") {
        CHECK(ResolveUniformMemberWriteSize(64).value() == 64u); // e.g. a mat4 member
    }

    TEST_CASE("zero reflected size has no safe write size to fall back to") {
        CHECK_FALSE(ResolveUniformMemberWriteSize(0).has_value());
    }
}
