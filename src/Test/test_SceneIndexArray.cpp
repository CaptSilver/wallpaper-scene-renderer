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

} // SceneIndexArray
