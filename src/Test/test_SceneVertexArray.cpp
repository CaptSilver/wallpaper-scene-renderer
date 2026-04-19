#include <doctest.h>

#include "Scene/SceneVertexArray.h"
#include "Type.hpp"

#include <array>
#include <string>
#include <vector>

using namespace wallpaper;

using Attr = SceneVertexArray::SceneVertexAttribute;

TEST_SUITE("SceneVertexArray.Statics") {

TEST_CASE("TypeCount returns component count per VertexType") {
    CHECK(SceneVertexArray::TypeCount(VertexType::FLOAT1) == 1);
    CHECK(SceneVertexArray::TypeCount(VertexType::FLOAT2) == 2);
    CHECK(SceneVertexArray::TypeCount(VertexType::FLOAT3) == 3);
    CHECK(SceneVertexArray::TypeCount(VertexType::FLOAT4) == 4);
    CHECK(SceneVertexArray::TypeCount(VertexType::UINT1) == 1);
    CHECK(SceneVertexArray::TypeCount(VertexType::UINT2) == 2);
    CHECK(SceneVertexArray::TypeCount(VertexType::UINT3) == 3);
    CHECK(SceneVertexArray::TypeCount(VertexType::UINT4) == 4);
}

TEST_CASE("RealAttributeSize pads to 4 when attr.padding is true") {
    Attr padded { .name = "p", .type = VertexType::FLOAT1, .padding = true };
    CHECK(SceneVertexArray::RealAttributeSize(padded) == 4);

    Attr padded3 { .name = "p", .type = VertexType::FLOAT3, .padding = true };
    CHECK(SceneVertexArray::RealAttributeSize(padded3) == 4);
}

TEST_CASE("RealAttributeSize returns raw type count when padding is false") {
    Attr raw2 { .name = "r", .type = VertexType::FLOAT2, .padding = false };
    CHECK(SceneVertexArray::RealAttributeSize(raw2) == 2);

    Attr raw4 { .name = "r", .type = VertexType::FLOAT4, .padding = false };
    CHECK(SceneVertexArray::RealAttributeSize(raw4) == 4);
}

} // Statics

TEST_SUITE("SceneVertexArray.Construction") {

TEST_CASE("Unpadded FLOAT3 gives oneSize=3, capacity=count*3, data zero-filled") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray arr(attrs, 4);
    CHECK(arr.OneSize() == 3);
    CHECK(arr.OneSizeOf() == 3 * sizeof(float));
    CHECK(arr.CapacitySize() == 12);
    CHECK(arr.CapacitySizeOf() == 12 * sizeof(float));
    CHECK(arr.VertexCount() == 0);
    CHECK(arr.DataSize() == 0);
    for (std::size_t i = 0; i < arr.CapacitySize(); ++i) {
        CHECK(arr.Data()[i] == 0.0f);
    }
}

TEST_CASE("Padded FLOAT2 pads up to 4-component stride") {
    std::vector<Attr> attrs {
        Attr { .name = "uv", .type = VertexType::FLOAT2, .padding = true }
    };
    SceneVertexArray arr(attrs, 2);
    CHECK(arr.OneSize() == 4);
    CHECK(arr.CapacitySize() == 8);
}

TEST_CASE("Mixed attribute layout computes stride across components") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false },
        Attr { .name = "uv", .type = VertexType::FLOAT2, .padding = false },
    };
    SceneVertexArray arr(attrs, 3);
    CHECK(arr.OneSize() == 5);
    CHECK(arr.CapacitySize() == 15);
}

} // Construction

TEST_SUITE("SceneVertexArray.AddVertex") {

TEST_CASE("AddVertex fills all slots up to capacity (off-by-one regression)") {
    // Before the fix, capacity=count*oneSize rejected the final vertex
    // because `m_size + m_oneSize >= m_capacity` returned on the last write.
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray arr(attrs, 3);
    CHECK(arr.CapacitySize() == 9);

    float v0[3] = { 1, 2, 3 };
    float v1[3] = { 4, 5, 6 };
    float v2[3] = { 7, 8, 9 };

    REQUIRE(arr.AddVertex(v0));
    REQUIRE(arr.AddVertex(v1));
    REQUIRE(arr.AddVertex(v2)); // regressed if false
    CHECK(arr.VertexCount() == 3);

    float next[3] = { 10, 11, 12 };
    CHECK_FALSE(arr.AddVertex(next)); // now truly at capacity
}

TEST_CASE("AddVertex with count=1 accepts one vertex") {
    // Previously: first AddVertex failed because 0 + 3 >= 3.
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray arr(attrs, 1);
    float            v[3] = { 1, 2, 3 };
    REQUIRE(arr.AddVertex(v));
    CHECK(arr.VertexCount() == 1);
    CHECK(arr.Data()[0] == 1);
    CHECK(arr.Data()[1] == 2);
    CHECK(arr.Data()[2] == 3);
}

TEST_CASE("AddVertex strips padding on write") {
    // Padded FLOAT2 takes 4 floats of storage per vertex but only 2
    // floats worth of input data is copied.
    std::vector<Attr> attrs {
        Attr { .name = "uv", .type = VertexType::FLOAT2, .padding = true }
    };
    SceneVertexArray arr(attrs, 2);
    float            uv0[2] = { 0.25f, 0.75f };
    REQUIRE(arr.AddVertex(uv0));
    CHECK(arr.Data()[0] == doctest::Approx(0.25f));
    CHECK(arr.Data()[1] == doctest::Approx(0.75f));
    // padding slots stay zero-initialized
    CHECK(arr.Data()[2] == 0.0f);
    CHECK(arr.Data()[3] == 0.0f);
}

} // AddVertex

TEST_SUITE("SceneVertexArray.SetVertex") {

TEST_CASE("SetVertex writes a full attribute column by name") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray          arr(attrs, 3);
    std::array<float, 9>      data { 1, 2, 3,   4, 5, 6,   7, 8, 9 };
    REQUIRE(arr.SetVertex("pos", std::span<const float>(data.data(), data.size())));
    CHECK(arr.VertexCount() == 3);
    for (std::size_t i = 0; i < data.size(); ++i) {
        CHECK(arr.Data()[i] == doctest::Approx(data[i]));
    }
}

TEST_CASE("SetVertex writes to correct offset for later attribute") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false },
        Attr { .name = "uv", .type = VertexType::FLOAT2, .padding = false },
    };
    SceneVertexArray     arr(attrs, 2);
    // Stride = 5 floats.  uv starts at offset 3.
    std::array<float, 4> uv { 0.0f, 0.1f, 0.2f, 0.3f };
    REQUIRE(arr.SetVertex("uv", std::span<const float>(uv.data(), uv.size())));
    CHECK(arr.Data()[3] == doctest::Approx(0.0f));
    CHECK(arr.Data()[4] == doctest::Approx(0.1f));
    CHECK(arr.Data()[3 + 5] == doctest::Approx(0.2f));
    CHECK(arr.Data()[4 + 5] == doctest::Approx(0.3f));
}

TEST_CASE("SetVertex with unknown attribute name returns false") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray     arr(attrs, 3);
    std::array<float, 3> data { 1, 2, 3 };
    CHECK_FALSE(arr.SetVertex("nope", std::span<const float>(data.data(), data.size())));
}

TEST_CASE("SetVertex beyond capacity returns false") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray     arr(attrs, 1); // capacity 3 floats
    std::array<float, 6> data { 1, 2, 3, 4, 5, 6 };
    CHECK_FALSE(arr.SetVertex("pos", std::span<const float>(data.data(), data.size())));
}

} // SetVertex

TEST_SUITE("SceneVertexArray.SetVertexs") {

TEST_CASE("SetVertexs at index 0 writes raw span") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray     arr(attrs, 2); // 6 floats
    std::array<float, 6> data { 1, 2, 3, 4, 5, 6 };
    REQUIRE(arr.SetVertexs(0, std::span<const float>(data.data(), data.size())));
    CHECK(arr.VertexCount() == 2);
    CHECK(arr.Data()[5] == doctest::Approx(6.f));
}

TEST_CASE("SetVertexs at index offset") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray     arr(attrs, 3);
    std::array<float, 3> data { 7, 8, 9 };
    REQUIRE(arr.SetVertexs(1, std::span<const float>(data.data(), data.size())));
    CHECK(arr.VertexCount() == 2); // wrote up to byte offset 3+3=6 → 2 vertices
    CHECK(arr.Data()[3] == doctest::Approx(7.f));
    CHECK(arr.Data()[5] == doctest::Approx(9.f));
}

TEST_CASE("SetVertexs beyond capacity returns false") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray     arr(attrs, 1);
    std::array<float, 6> data { 1, 2, 3, 4, 5, 6 };
    CHECK_FALSE(arr.SetVertexs(0, std::span<const float>(data.data(), data.size())));
}

} // SetVertexs

TEST_SUITE("SceneVertexArray.AttrOffsetMap") {

TEST_CASE("GetAttrOffsetMap reports byte offsets in stride order") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false },
        Attr { .name = "uv", .type = VertexType::FLOAT2, .padding = false },
        Attr { .name = "color", .type = VertexType::FLOAT4, .padding = true },
    };
    SceneVertexArray arr(attrs, 1);
    auto             m = arr.GetAttrOffsetMap();
    REQUIRE(m.count("pos") == 1);
    REQUIRE(m.count("uv") == 1);
    REQUIRE(m.count("color") == 1);
    CHECK(m.at("pos").offset == 0);
    CHECK(m.at("uv").offset == 3 * sizeof(float));
    CHECK(m.at("color").offset == (3 + 2) * sizeof(float));
}

} // AttrOffsetMap

TEST_SUITE("SceneVertexArray.Options") {

TEST_CASE("GetOption on unset key is false") {
    SceneVertexArray arr({ Attr { .name = "p", .type = VertexType::FLOAT1, .padding = false } }, 1);
    CHECK_FALSE(arr.GetOption("anything"));
}

TEST_CASE("SetOption then GetOption roundtrips") {
    SceneVertexArray arr({ Attr { .name = "p", .type = VertexType::FLOAT1, .padding = false } }, 1);
    arr.SetOption("key", true);
    CHECK(arr.GetOption("key"));
    arr.SetOption("key", false);
    CHECK_FALSE(arr.GetOption("key"));
}

} // Options

TEST_SUITE("SceneVertexArray.Render") {

TEST_CASE("RenderVertexCount falls back to VertexCount when unset") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray     arr(attrs, 2);
    std::array<float, 6> data { 1, 2, 3, 4, 5, 6 };
    arr.SetVertexs(0, std::span<const float>(data.data(), data.size()));
    CHECK(arr.RenderVertexCount() == 2);
}

TEST_CASE("SetRenderVertexCount overrides when > 0") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray     arr(attrs, 4);
    std::array<float, 9> data { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    arr.SetVertexs(0, std::span<const float>(data.data(), data.size()));
    arr.SetRenderVertexCount(1);
    CHECK(arr.RenderVertexCount() == 1);
}

TEST_CASE("SetRenderVertexCount(0) returns to VertexCount fallback") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray     arr(attrs, 2);
    std::array<float, 6> data { 1, 2, 3, 4, 5, 6 };
    arr.SetVertexs(0, std::span<const float>(data.data(), data.size()));
    arr.SetRenderVertexCount(1);
    arr.SetRenderVertexCount(0);
    CHECK(arr.RenderVertexCount() == 2);
}

} // Render

TEST_SUITE("SceneVertexArray.Lifecycle") {

TEST_CASE("move constructor transfers ownership") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT3, .padding = false }
    };
    SceneVertexArray     a(attrs, 2);
    std::array<float, 6> data { 1, 2, 3, 4, 5, 6 };
    a.SetVertexs(0, std::span<const float>(data.data(), data.size()));
    a.SetID(77);

    SceneVertexArray b(std::move(a));
    CHECK(b.VertexCount() == 2);
    CHECK(b.ID() == 77u);
    CHECK(b.Data()[0] == doctest::Approx(1.f));
    CHECK(b.Data()[5] == doctest::Approx(6.f));
}

TEST_CASE("move assignment transfers ownership") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT1, .padding = false }
    };
    SceneVertexArray     a(attrs, 4);
    std::array<float, 3> da { 10, 20, 30 };
    a.SetVertexs(0, std::span<const float>(da.data(), da.size()));
    a.SetID(1);

    SceneVertexArray     b(attrs, 1);
    std::array<float, 1> db { 99 };
    b.SetVertexs(0, std::span<const float>(db.data(), db.size()));
    b.SetID(2);

    b = std::move(a);
    CHECK(b.VertexCount() == 3);
    CHECK(b.ID() == 1u);
    CHECK(b.Data()[0] == doctest::Approx(10.f));
    CHECK(b.Data()[2] == doctest::Approx(30.f));
}

TEST_CASE("ID setter/getter roundtrip") {
    std::vector<Attr> attrs {
        Attr { .name = "pos", .type = VertexType::FLOAT1, .padding = false }
    };
    SceneVertexArray arr(attrs, 1);
    arr.SetID(0xF00D);
    CHECK(arr.ID() == 0xF00Du);
}

} // Lifecycle
