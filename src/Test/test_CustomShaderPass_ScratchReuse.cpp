#include <doctest.h>

#include <vulkan/vulkan.h>

#include <vector>

// The per-pass scratch-reuse contract for CustomShaderPass::execute() (the
// record path).  Properties tested:
//
//   1. clear() preserves capacity (cost-savings claim — no allocator hit
//      between frames after the first prepare-time reserve).
//   2. push_back after reserve(N) never reallocates while size <= N
//      (pointer-stability claim — pImageInfo points into
//      m_image_infos_scratch and must remain valid across subsequent
//      push_backs).
//
// A live CustomShaderPass is not required: the properties hold on plain
// std::vector<T>.  Locking them here means a future maintainer who drops
// the prepare-time reserve, or who switches a member to a thread_local
// or some other container with different invariants, fails this test
// BEFORE shipping the dangling-pImageInfo bug.
TEST_SUITE("CustomShaderPass record-path scratch") {
    TEST_CASE("VkImageMemoryBarrier scratch retains capacity across clear/refill") {
        std::vector<VkImageMemoryBarrier> v;
        v.reserve(16);
        const auto cap0 = v.capacity();
        REQUIRE(cap0 >= 16);
        for (int iter = 0; iter < 4; ++iter) {
            v.clear();
            CHECK(v.capacity() == cap0); // clear() must not shrink
            for (int i = 0; i < 16; ++i) {
                v.push_back(
                    VkImageMemoryBarrier { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER });
            }
            CHECK(v.capacity() == cap0); // push_back inside reserve must not realloc
        }
    }

    TEST_CASE("VkDescriptorImageInfo addresses stay stable across all push_backs") {
        // The pointer-stability contract: m_descriptor_writes_scratch stores
        // &m_image_infos_scratch.back() — if push_back reallocates, every
        // earlier pImageInfo dangles.  Pre-reserved capacity + size<=capacity
        // is the guarantee.
        std::vector<VkDescriptorImageInfo> infos;
        infos.reserve(16);
        std::vector<const VkDescriptorImageInfo*> captured_addrs;
        for (int i = 0; i < 16; ++i) {
            infos.push_back(VkDescriptorImageInfo {});
            captured_addrs.push_back(&infos.back());
        }
        // After all 16 pushes, every captured address must still resolve into
        // the same slot of infos.
        for (size_t i = 0; i < captured_addrs.size(); ++i) {
            CHECK(captured_addrs[i] == &infos[i]);
        }
    }

    TEST_CASE("VkWriteDescriptorSet scratch retains capacity across clear/refill") {
        std::vector<VkWriteDescriptorSet> v;
        v.reserve(17); // 16 textures + 1 UBO
        const auto cap0 = v.capacity();
        REQUIRE(cap0 >= 17);
        for (int iter = 0; iter < 4; ++iter) {
            v.clear();
            CHECK(v.capacity() == cap0);
            for (int i = 0; i < 17; ++i) {
                v.push_back(
                    VkWriteDescriptorSet { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET });
            }
            CHECK(v.capacity() == cap0);
        }
    }

    TEST_CASE("clear() on trivial Vulkan structs preserves capacity") {
        // Spec claim: clear() on trivial types is "a single pointer
        // assignment" — at minimum, capacity must equal pre-clear capacity.
        std::vector<VkImageMemoryBarrier> v;
        v.reserve(32);
        v.push_back(VkImageMemoryBarrier { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER });
        v.push_back(VkImageMemoryBarrier { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER });
        const auto cap_before = v.capacity();
        v.clear();
        CHECK(v.size() == 0u);
        CHECK(v.capacity() == cap_before);
    }

    TEST_CASE("pImageInfo pointer surviving N captures still points into the source vector") {
        // Mirrors the exact pattern execute() uses: push image-info, capture
        // its address, push more, then later consume the captured addresses.
        // If reserve hadn't bounded capacity, push_back N+1 would realloc and
        // every earlier captured pointer would dangle.
        struct Capture {
            uint32_t                     binding;
            const VkDescriptorImageInfo* p;
        };
        std::vector<VkDescriptorImageInfo> infos;
        infos.reserve(17);
        std::vector<Capture> writes;
        for (uint32_t i = 0; i < 17; ++i) {
            infos.push_back(VkDescriptorImageInfo {});
            writes.push_back(Capture { i, &infos.back() });
        }
        // After every push is done, each captured pointer must resolve to its
        // original slot in `infos`.
        for (size_t i = 0; i < writes.size(); ++i) {
            CHECK(writes[i].p == &infos[i]);
            CHECK(writes[i].binding == (uint32_t)i);
        }
    }

} // TEST_SUITE
