#include <doctest.h>

#include "Scene/SceneIndexArray.h"

#include <array>
#include <cstdint>
#include <cstring>

using namespace wallpaper;

TEST_SUITE("SceneIndexArray") {
    TEST_CASE("count constructor sets capacity to count * 3 and zero size") {
        // Constructor takes index *triangles* count — internal capacity is
        // count*3 (three indices per triangle).
        SceneIndexArray arr(4);
        CHECK(arr.DataCount() == 0);
        CHECK(arr.CapacityCount() == 12);
        CHECK(arr.CapacitySizeof() == 12 * sizeof(uint32_t));
        CHECK(arr.DataSizeOf() == 0);
    }

    TEST_CASE("count constructor zero-initializes the buffer") {
        SceneIndexArray arr(2);
        const uint32_t* p = arr.Data();
        REQUIRE(p != nullptr);
        for (std::size_t i = 0; i < arr.CapacityCount(); ++i) {
            CHECK(p[i] == 0u);
        }
    }

    TEST_CASE("span constructor copies data and sets size=capacity") {
        std::array<uint32_t, 5> src { 10, 20, 30, 40, 50 };
        SceneIndexArray         arr(std::span<const uint32_t>(src.data(), src.size()));
        CHECK(arr.DataCount() == 5);
        CHECK(arr.CapacityCount() == 5);
        CHECK(arr.DataSizeOf() == 5 * sizeof(uint32_t));
        REQUIRE(arr.Data() != nullptr);
        for (std::size_t i = 0; i < src.size(); ++i) {
            CHECK(arr.Data()[i] == src[i]);
        }
    }

    TEST_CASE("Assign within capacity writes data and grows size") {
        SceneIndexArray         arr(4); // capacity 12
        std::array<uint32_t, 6> data { 1, 2, 3, 4, 5, 6 };
        arr.Assign(0, std::span<const uint32_t>(data.data(), data.size()));
        CHECK(arr.DataCount() == 6);
        for (std::size_t i = 0; i < data.size(); ++i) {
            CHECK(arr.Data()[i] == data[i]);
        }
    }

    TEST_CASE("Assign at non-zero index offsets correctly") {
        SceneIndexArray         arr(4); // capacity 12
        std::array<uint32_t, 3> data { 7, 8, 9 };
        arr.Assign(4, std::span<const uint32_t>(data.data(), data.size()));
        CHECK(arr.DataCount() == 7);
        CHECK(arr.Data()[4] == 7u);
        CHECK(arr.Data()[5] == 8u);
        CHECK(arr.Data()[6] == 9u);
    }

    TEST_CASE("Assign that would exceed capacity is silently ignored") {
        SceneIndexArray         arr(1); // capacity 3
        std::array<uint32_t, 5> big { 1, 2, 3, 4, 5 };
        arr.Assign(0, std::span<const uint32_t>(big.data(), big.size()));
        // IncreaseCheckSet returned false → size stays 0, buffer untouched
        CHECK(arr.DataCount() == 0);
        CHECK(arr.Data()[0] == 0u);
    }

    TEST_CASE("Assign exactly fills capacity") {
        SceneIndexArray         arr(2); // capacity 6
        std::array<uint32_t, 6> data { 1, 2, 3, 4, 5, 6 };
        arr.Assign(0, std::span<const uint32_t>(data.data(), data.size()));
        CHECK(arr.DataCount() == 6);
        CHECK(arr.DataSizeOf() == arr.CapacitySizeof());
    }

    TEST_CASE("AssignHalf writes 16-bit indices into the buffer") {
        SceneIndexArray         arr(2); // capacity 6 uint32_t = 24 bytes
        std::array<uint16_t, 4> halves { 0xAAAA, 0xBBBB, 0xCCCC, 0xDDDD };
        arr.AssignHalf(0, std::span<const uint16_t>(halves.data(), halves.size()));
        // 4 halves = 8 bytes; rounded up to 2 uint32_t units.
        CHECK(arr.DataCount() == 2);
        const auto* p = reinterpret_cast<const uint16_t*>(arr.Data());
        for (std::size_t i = 0; i < halves.size(); ++i) {
            CHECK(p[i] == halves[i]);
        }
    }

    TEST_CASE("AssignHalf beyond capacity is ignored") {
        SceneIndexArray         arr(1); // capacity 3 uint32_t = 12 bytes = 6 uint16_t
        std::array<uint16_t, 7> too_many { 1, 2, 3, 4, 5, 6, 7 };
        arr.AssignHalf(0, std::span<const uint16_t>(too_many.data(), too_many.size()));
        CHECK(arr.DataCount() == 0);
    }

    TEST_CASE("RenderDataCount defaults to size (unlimited render range)") {
        SceneIndexArray         arr(4);
        std::array<uint32_t, 3> data { 1, 2, 3 };
        arr.Assign(0, std::span<const uint32_t>(data.data(), data.size()));
        // Default m_render_size is max<size_t>, capped to m_size
        CHECK(arr.RenderDataCount() == 3);
    }

    TEST_CASE("SetRenderDataCount clamps to actual data size") {
        SceneIndexArray         arr(4);
        std::array<uint32_t, 6> data { 1, 2, 3, 4, 5, 6 };
        arr.Assign(0, std::span<const uint32_t>(data.data(), data.size()));

        arr.SetRenderDataCount(3);
        CHECK(arr.RenderDataCount() == 3);

        // Larger than data → clamp to data size
        arr.SetRenderDataCount(100);
        CHECK(arr.RenderDataCount() == 6);
    }

    TEST_CASE("ID setter and getter") {
        SceneIndexArray arr(1);
        arr.SetID(0xCAFE);
        CHECK(arr.ID() == 0xCAFEu);
    }

    TEST_CASE("move constructor transfers ownership") {
        std::array<uint32_t, 4> src { 9, 8, 7, 6 };
        SceneIndexArray         a(std::span<const uint32_t>(src.data(), src.size()));
        a.SetID(123);

        SceneIndexArray b(std::move(a));
        CHECK(b.DataCount() == 4);
        CHECK(b.ID() == 123u);
        for (std::size_t i = 0; i < src.size(); ++i) CHECK(b.Data()[i] == src[i]);
    }

    TEST_CASE("count ctor capacity multiplier pins * 3 (not / 3 or + 3)") {
        // With indexCount=7, only `* 3` yields 21.  `indexCount / 3 = 2`,
        // `indexCount + 3 = 10` both wrong.  Varied sizes distinguish every
        // arithmetic mutation on the multiplier expression.
        SceneIndexArray arr1(1);
        SceneIndexArray arr2(7);
        SceneIndexArray arr3(13);
        CHECK(arr1.CapacityCount() == 3u);
        CHECK(arr2.CapacityCount() == 21u);
        CHECK(arr3.CapacityCount() == 39u);
    }

    TEST_CASE("AssignHalf with non-aligned byte length rounds UP via +1 carry") {
        // 7 uint16 halves = 14 bytes.  m_size = 14/4 + (14%4 != 0 ? 1 : 0)
        // = 3 + 1 = 4 uint32 units.  Without the `+1` (mutation `+ → -`),
        // m_size would be 3-1=2, and DataCount would misreport.
        SceneIndexArray         arr(2); // capacity 6 uint32_t = 24 bytes = enough for 14
        std::array<uint16_t, 7> odd { 1, 2, 3, 4, 5, 6, 7 };
        arr.AssignHalf(0, std::span<const uint16_t>(odd.data(), odd.size()));
        // 7 * 2 = 14 bytes → 4 uint32 units (round up from 3.5).
        CHECK(arr.DataCount() == 4u);
        CHECK(arr.DataSizeOf() == 4u * sizeof(uint32_t));
    }

    TEST_CASE("AssignHalf with exactly 4-byte-aligned length does NOT carry") {
        // 8 halves = 16 bytes → exactly 4 uint32 units; the `(nsize%4 == 0 ? 0 : 1)`
        // branch must select 0.  Mutation `== → !=` flips the carry and yields 5.
        SceneIndexArray         arr(2); // capacity 6 uint32_t = 24 bytes
        std::array<uint16_t, 8> aligned { 1, 2, 3, 4, 5, 6, 7, 8 };
        arr.AssignHalf(0, std::span<const uint16_t>(aligned.data(), aligned.size()));
        CHECK(arr.DataCount() == 4u);
    }

    TEST_CASE("Assign exactly at capacity still writes (boundary on > vs >=)") {
        // nsize == CapacitySizeof() must NOT be rejected: `>` not `>=`.
        // 3 uint32 = 12 bytes = exactly capacity of SceneIndexArray(1) (=3*4).
        SceneIndexArray         arr(1);
        std::array<uint32_t, 3> full { 100, 200, 300 };
        arr.Assign(0, std::span<const uint32_t>(full.data(), full.size()));
        CHECK(arr.DataCount() == 3u);
        CHECK(arr.Data()[0] == 100u);
        CHECK(arr.Data()[2] == 300u);
    }

} // SceneIndexArray
