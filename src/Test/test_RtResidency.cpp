#include <doctest.h>

#include "VulkanRender/RtResidency.hpp"

#include <cstdint>
#include <unordered_set>

using namespace wallpaper::vulkan;

// Keys for the pass analyses: a render target is named by its slot id, a depth
// or MSAA image by its VkImage handle.  The two kinds must never compare equal,
// and the default key must be the null every PassCacheAlias / PassMerge
// template compares against.
TEST_SUITE("RtResidency::ResidencyKey") {
    TEST_CASE("the default key is null and compares equal to another default key") {
        ResidencyKey a, b;
        CHECK(a == b);
        CHECK(a.kind == ResidencyKey::Kind::None);
        CHECK(a == ResidencyKey {});
    }
    TEST_CASE("a slot key and a handle key with the same value never compare equal") {
        CHECK_FALSE(ResidencyKey::ForSlot(7) == ResidencyKey::ForHandle(7));
        CHECK_FALSE(ResidencyKey::ForSlot(0) == ResidencyKey {});
        CHECK(ResidencyKey::ForSlot(7) == ResidencyKey::ForSlot(7));
        CHECK_FALSE(ResidencyKey::ForSlot(7) == ResidencyKey::ForSlot(8));
    }
    TEST_CASE("a null handle gives the null key, so unbound slots stay invisible to the analyses") {
        CHECK(ResidencyKey::ForHandle(0) == ResidencyKey {});
        CHECK(KeyForImage(static_cast<std::uintptr_t>(0)) == ResidencyKey {});
        CHECK(KeyForImage(static_cast<std::uintptr_t>(0x1234)) == ResidencyKey::ForHandle(0x1234));
    }
    TEST_CASE("a pointer handle maps through its address and a null pointer is the null key") {
        CHECK(KeyForImage(static_cast<int*>(nullptr)) == ResidencyKey {});
        CHECK(KeyForImage(reinterpret_cast<int*>(std::uintptr_t { 0x1234 })) ==
              ResidencyKey::ForHandle(0x1234));
    }
    TEST_CASE("keys hash into an unordered_set without collapsing kinds") {
        std::unordered_set<ResidencyKey> set;
        set.insert(ResidencyKey::ForSlot(1));
        set.insert(ResidencyKey::ForHandle(1));
        set.insert(ResidencyKey::ForSlot(1));
        CHECK(set.size() == 2);
        CHECK(set.count(ResidencyKey::ForSlot(1)) == 1);
        CHECK(set.count(ResidencyKey::ForHandle(1)) == 1);
        CHECK(set.count(ResidencyKey {}) == 0);
    }
}
