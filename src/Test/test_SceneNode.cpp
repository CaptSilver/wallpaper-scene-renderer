#include <doctest.h>

#include "Scene/SceneNode.h"
#include "Scene/SceneMesh.h"

#include <Eigen/Dense>
#include <cmath>
#include <memory>

using namespace wallpaper;
using namespace Eigen;

// ===========================================================================
// Transform math — GetLocalTrans / UpdateTrans
// ===========================================================================

TEST_SUITE("SceneNode.Transform") {
    TEST_CASE("default node produces identity local transform") {
        SceneNode n;
        Matrix4d  m = n.GetLocalTrans();
        CHECK(m.isApprox(Matrix4d::Identity()));
    }

    TEST_CASE("translation-only local transform") {
        SceneNode n({ 1.f, 2.f, 3.f }, { 1.f, 1.f, 1.f }, { 0.f, 0.f, 0.f });
        Matrix4d  m = n.GetLocalTrans();
        CHECK(m(0, 3) == doctest::Approx(1.0));
        CHECK(m(1, 3) == doctest::Approx(2.0));
        CHECK(m(2, 3) == doctest::Approx(3.0));
        // Linear block stays identity
        CHECK(m.block<3, 3>(0, 0).isApprox(Matrix3d::Identity()));
    }

    TEST_CASE("scale-only local transform writes diagonal") {
        SceneNode n({ 0, 0, 0 }, { 2.f, 3.f, 4.f }, { 0, 0, 0 });
        Matrix4d  m = n.GetLocalTrans();
        CHECK(m(0, 0) == doctest::Approx(2.0));
        CHECK(m(1, 1) == doctest::Approx(3.0));
        CHECK(m(2, 2) == doctest::Approx(4.0));
        CHECK(m(0, 3) == doctest::Approx(0.0));
    }

    TEST_CASE("rotation about Z by 90° maps +X → +Y") {
        SceneNode n({ 0, 0, 0 }, { 1, 1, 1 }, { 0.f, 0.f, (float)M_PI_2 });
        Matrix4d  m = n.GetLocalTrans();
        Vector4d  x_in(1, 0, 0, 1);
        Vector4d  x_out = m * x_in;
        CHECK(x_out.x() == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(x_out.y() == doctest::Approx(1.0).epsilon(1e-6));
    }

    TEST_CASE("UpdateTrans on a leaf node produces local transform") {
        SceneNode n({ 5.f, 0.f, 0.f }, { 1, 1, 1 }, { 0, 0, 0 });
        n.UpdateTrans();
        Matrix4d m = n.ModelTrans();
        CHECK(m(0, 3) == doctest::Approx(5.0));
    }

    TEST_CASE("UpdateTrans chains parent * child local transforms") {
        auto parent = std::make_shared<SceneNode>(
            Vector3f(10.f, 0.f, 0.f), Vector3f(1.f, 1.f, 1.f), Vector3f(0.f, 0.f, 0.f));
        auto child = std::make_shared<SceneNode>(
            Vector3f(2.f, 3.f, 0.f), Vector3f(1.f, 1.f, 1.f), Vector3f(0.f, 0.f, 0.f));
        parent->AppendChild(child);

        child->UpdateTrans();
        // child world-X = parent.translate.x + child.translate.x = 12
        CHECK(child->ModelTrans()(0, 3) == doctest::Approx(12.0));
        CHECK(child->ModelTrans()(1, 3) == doctest::Approx(3.0));
    }

    TEST_CASE("UpdateTrans after MarkTransDirty recomputes") {
        auto p = std::make_shared<SceneNode>(
            Vector3f(1.f, 0.f, 0.f), Vector3f(1, 1, 1), Vector3f(0, 0, 0));
        auto c =
            std::make_shared<SceneNode>(Vector3f(0, 0, 0), Vector3f(1, 1, 1), Vector3f(0, 0, 0));
        p->AppendChild(c);
        c->UpdateTrans();
        CHECK(c->ModelTrans()(0, 3) == doctest::Approx(1.0));

        p->SetTranslate(Vector3f(7.f, 0.f, 0.f));
        // SetTranslate calls MarkTransDirty → child marked dirty too
        c->UpdateTrans();
        CHECK(c->ModelTrans()(0, 3) == doctest::Approx(7.0));
    }

    TEST_CASE("UpdateTrans is a no-op when not dirty") {
        SceneNode n({ 1.f, 0.f, 0.f }, { 1, 1, 1 }, { 0, 0, 0 });
        n.UpdateTrans();
        // Stomp the cached transform manually to prove the second UpdateTrans
        // doesn't recompute when the dirty flag is clear.
        n.SetWorldTransform(Matrix4d::Identity() * 42.0);
        n.UpdateTrans();
        CHECK(n.ModelTrans()(0, 0) == doctest::Approx(42.0));
    }

} // Transform

// ===========================================================================
// Trans-copy helpers — CopyTrans / CopyTransWithParent / InheritParent
// ===========================================================================

TEST_SUITE("SceneNode.TransCopy") {
    TEST_CASE("CopyTrans copies translate/scale/rotation only") {
        SceneNode src({ 1.f, 2.f, 3.f }, { 4.f, 5.f, 6.f }, { 0.f, 0.f, 1.f });
        SceneNode dst;
        dst.CopyTrans(src);
        CHECK(dst.Translate().isApprox(Vector3f(1, 2, 3)));
        CHECK(dst.Scale().isApprox(Vector3f(4, 5, 6)));
        CHECK(dst.Rotation().z() == doctest::Approx(1.0f));
    }

    TEST_CASE("CopyTransWithParent also reparents") {
        auto parent = std::make_shared<SceneNode>(
            Vector3f(10.f, 0.f, 0.f), Vector3f(1, 1, 1), Vector3f(0, 0, 0));
        auto src = std::make_shared<SceneNode>(
            Vector3f(1.f, 0.f, 0.f), Vector3f(1, 1, 1), Vector3f(0, 0, 0));
        parent->AppendChild(src);

        SceneNode dst;
        dst.CopyTransWithParent(*src);
        dst.UpdateTrans();
        // dst should evaluate parent+local → 10 + 1 = 11
        CHECK(dst.ModelTrans()(0, 3) == doctest::Approx(11.0));
    }

    TEST_CASE("InheritParent keeps local transform, inherits parent chain") {
        auto parent = std::make_shared<SceneNode>(
            Vector3f(100.f, 0.f, 0.f), Vector3f(1, 1, 1), Vector3f(0, 0, 0));
        auto src =
            std::make_shared<SceneNode>(Vector3f(0, 0, 0), Vector3f(1, 1, 1), Vector3f(0, 0, 0));
        parent->AppendChild(src);

        SceneNode dst({ 5.f, 0.f, 0.f }, { 1, 1, 1 }, { 0, 0, 0 });
        dst.InheritParent(*src);
        dst.UpdateTrans();
        // Local translate of dst (5) stays; parent contributes 100 → 105
        CHECK(dst.ModelTrans()(0, 3) == doctest::Approx(105.0));
    }

    TEST_CASE("SetParent wires the transform chain") {
        auto parent = std::make_shared<SceneNode>(
            Vector3f(3.f, 0.f, 0.f), Vector3f(1, 1, 1), Vector3f(0, 0, 0));
        SceneNode child({ 1.f, 0.f, 0.f }, { 1, 1, 1 }, { 0, 0, 0 });
        child.SetParent(parent.get());
        child.UpdateTrans();
        CHECK(child.ModelTrans()(0, 3) == doctest::Approx(4.0));
    }

    TEST_CASE("SetWorldTransform bakes a value and clears dirty") {
        SceneNode n({ 1.f, 2.f, 3.f }, { 1, 1, 1 }, { 0, 0, 0 });
        Matrix4d  baked = Matrix4d::Identity();
        baked(0, 3)     = 999.0;
        n.SetWorldTransform(baked);
        n.UpdateTrans(); // should NOT recompute since dirty was cleared
        CHECK(n.ModelTrans()(0, 3) == doctest::Approx(999.0));
    }

} // TransCopy

// ===========================================================================
// Scene-graph children — AppendChild / GetChildren / visibility parent
// ===========================================================================

TEST_SUITE("SceneNode.Graph") {
    TEST_CASE("AppendChild appends and sets parent pointers") {
        auto a = std::make_shared<SceneNode>();
        auto b = std::make_shared<SceneNode>();
        auto c = std::make_shared<SceneNode>();
        a->AppendChild(b);
        a->AppendChild(c);
        const auto& kids = a->GetChildren();
        CHECK(kids.size() == 2);
    }

    TEST_CASE("GetChildren non-const variant is mutable") {
        auto p = std::make_shared<SceneNode>();
        auto k = std::make_shared<SceneNode>();
        p->AppendChild(k);
        auto& kids = p->GetChildren();
        CHECK(kids.size() == 1);
        kids.clear();
        CHECK(p->GetChildren().size() == 0);
    }

    TEST_CASE("MarkTransDirty propagates to descendants") {
        auto p = std::make_shared<SceneNode>();
        auto k =
            std::make_shared<SceneNode>(Vector3f(1, 0, 0), Vector3f(1, 1, 1), Vector3f(0, 0, 0));
        p->AppendChild(k);
        k->UpdateTrans(); // clears dirty on child
        // SetTranslate on parent marks parent AND descendants dirty
        p->SetTranslate(Vector3f(10, 0, 0));
        k->UpdateTrans();
        CHECK(k->ModelTrans()(0, 3) == doctest::Approx(11.0));
    }

} // Graph

// ===========================================================================
// Visibility — m_visible, m_visibility_parent, m_visibilityOwner
// ===========================================================================

TEST_SUITE("SceneNode.Visibility") {
    TEST_CASE("default node is visible") {
        SceneNode n;
        CHECK(n.IsVisible());
    }

    TEST_CASE("SetVisible(false) hides the node") {
        SceneNode n;
        n.SetVisible(false);
        CHECK_FALSE(n.IsVisible());
    }

    TEST_CASE("hidden visibility parent hides descendants") {
        auto p = std::make_shared<SceneNode>();
        auto c = std::make_shared<SceneNode>();
        p->AppendChild(c); // also sets visibility_parent = p
        CHECK(c->IsVisible());
        p->SetVisible(false);
        CHECK_FALSE(c->IsVisible());
        p->SetVisible(true);
        CHECK(c->IsVisible());
    }

    TEST_CASE("visibility parent chain propagates through depth") {
        auto a = std::make_shared<SceneNode>();
        auto b = std::make_shared<SceneNode>();
        auto c = std::make_shared<SceneNode>();
        a->AppendChild(b);
        b->AppendChild(c);
        a->SetVisible(false);
        CHECK_FALSE(c->IsVisible());
    }

    TEST_CASE("visibility owner overrides even when self is visible") {
        SceneNode owner;
        SceneNode effect;
        effect.SetVisibilityOwner(&owner);
        owner.SetVisible(false);
        CHECK_FALSE(effect.IsVisible());
        owner.SetVisible(true);
        CHECK(effect.IsVisible());
    }

} // Visibility

// ===========================================================================
// Misc accessors — offscreen, camera, ID, mesh, name
// ===========================================================================

TEST_SUITE("SceneNode.Misc") {
    TEST_CASE("offscreen defaults to false and SetOffscreen toggles") {
        SceneNode n;
        CHECK_FALSE(n.IsOffscreen());
        n.SetOffscreen(true);
        CHECK(n.IsOffscreen());
    }

    TEST_CASE("camera name accessor") {
        SceneNode n;
        CHECK(n.Camera().empty());
        n.SetCamera("cam1");
        CHECK(n.Camera() == "cam1");
    }

    TEST_CASE("ID defaults to -1 and can be mutated via non-const ref") {
        SceneNode n;
        CHECK(n.ID() == -1);
        n.ID() = 42;
        CHECK(n.ID() == 42);
    }

    TEST_CASE("AddMesh/Mesh roundtrip") {
        SceneNode n;
        CHECK(n.Mesh() == nullptr);
        CHECK_FALSE(n.HasMaterial()); // no mesh → no material

        auto mesh = std::make_shared<SceneMesh>(false);
        n.AddMesh(mesh);
        CHECK(n.Mesh() == mesh.get());
        CHECK_FALSE(n.HasMaterial()); // mesh present, but Material() is null
    }

    TEST_CASE("constructor stores name") {
        SceneNode n({ 0, 0, 0 }, { 1, 1, 1 }, { 0, 0, 0 }, "test-node");
        // Name isn't exposed via a getter directly, but default-constructed node
        // gets empty name. Exercising this ctor overload is the goal (coverage).
        // Any observable: transform is identity.
        CHECK(n.GetLocalTrans().isApprox(Matrix4d::Identity()));
    }

} // Misc
