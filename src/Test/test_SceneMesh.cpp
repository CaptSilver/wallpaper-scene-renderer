#include <doctest.h>

#include "Scene/SceneMesh.h"
#include "Scene/SceneVertexArray.h"
#include "Scene/SceneIndexArray.h"
#include "Scene/SceneMaterial.h"
#include "Type.hpp"

#include <array>
#include <cstdint>

using namespace wallpaper;

using Attr = SceneVertexArray::SceneVertexAttribute;

TEST_SUITE("SceneMesh") {
    TEST_CASE(
        "default-constructed mesh: no arrays, no material, TRIANGLE primitive, point size 1") {
        SceneMesh m;
        CHECK(m.VertexCount() == 0);
        CHECK(m.IndexCount() == 0);
        CHECK(m.Primitive() == MeshPrimitive::TRIANGLE);
        CHECK(m.PointSize() == 1u);
        CHECK_FALSE(m.Dynamic());
        CHECK(m.Material() == nullptr);
    }

    TEST_CASE("dynamic-flag constructor") {
        SceneMesh m(true);
        CHECK(m.Dynamic());
    }

    TEST_CASE("AddVertexArray/AddIndexArray grow the internal vectors") {
        SceneMesh         m;
        std::vector<Attr> attrs { Attr {
            .name = "pos", .type = VertexType::FLOAT3, .padding = false } };
        m.AddVertexArray(SceneVertexArray(attrs, 1));
        CHECK(m.VertexCount() == 1);
        m.AddIndexArray(SceneIndexArray(3));
        CHECK(m.IndexCount() == 1);
    }

    TEST_CASE("GetVertexArray/GetIndexArray return reference to stored data") {
        SceneMesh         m;
        std::vector<Attr> attrs { Attr {
            .name = "p", .type = VertexType::FLOAT1, .padding = false } };
        SceneVertexArray  v(attrs, 1);
        v.SetID(99);
        m.AddVertexArray(std::move(v));
        CHECK(m.GetVertexArray(0).ID() == 99u);

        SceneIndexArray i(1);
        i.SetID(77);
        m.AddIndexArray(std::move(i));
        CHECK(m.GetIndexArray(0).ID() == 77u);
    }

    TEST_CASE("SetPrimitive / SetPointSize accessors") {
        SceneMesh m;
        m.SetPrimitive(MeshPrimitive::POINT);
        CHECK(m.Primitive() == MeshPrimitive::POINT);
        m.SetPointSize(16);
        CHECK(m.PointSize() == 16u);
    }

    TEST_CASE("ID setter and getter") {
        SceneMesh m;
        m.SetID(12345);
        CHECK(m.ID() == 12345u);
    }

    TEST_CASE("AddMaterial populates Material() pointer") {
        SceneMesh m;
        CHECK(m.Material() == nullptr);
        SceneMaterial mat;
        mat.name = "shiny";
        m.AddMaterial(std::move(mat));
        REQUIRE(m.Material() != nullptr);
        CHECK(m.Material()->name == "shiny");
    }

    TEST_CASE("Dirty flag is atomic, accessible mutably and immutably") {
        SceneMesh m;
        CHECK_FALSE(m.Dirty().load());
        m.SetDirty();
        CHECK(m.Dirty().load());
        // Mutable ref
        m.Dirty().store(false);
        CHECK_FALSE(m.Dirty().load());
    }

    TEST_CASE("ChangeMeshDataFrom shares vertex/index data with another mesh") {
        SceneMesh         a, b;
        std::vector<Attr> attrs { Attr {
            .name = "p", .type = VertexType::FLOAT1, .padding = false } };
        a.AddVertexArray(SceneVertexArray(attrs, 1));
        a.AddIndexArray(SceneIndexArray(1));
        b.ChangeMeshDataFrom(a);
        // Both meshes now see the same underlying vertex/index storage.
        CHECK(b.VertexCount() == 1);
        CHECK(b.IndexCount() == 1);
    }

} // SceneMesh
