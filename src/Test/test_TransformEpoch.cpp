#include <doctest.h>

#include "Scene/SceneNode.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneShader.h"

#include <Eigen/Dense>
#include <memory>

using namespace wallpaper;
using namespace Eigen;

TEST_SUITE("SceneNode transform epoch") {
    TEST_CASE("UpdateTrans bumps the epoch only on the recompute (dirty) path") {
        SceneNode n({ 1.f, 0.f, 0.f }, { 1, 1, 1 }, { 0, 0, 0 });
        uint64_t  e0 = n.TransEpoch();
        n.UpdateTrans(); // dirty -> recompute -> bump
        uint64_t e1 = n.TransEpoch();
        CHECK(e1 != e0);
        n.UpdateTrans(); // clean -> no-op -> no bump
        CHECK(n.TransEpoch() == e1);
    }

    TEST_CASE("SetTranslate then UpdateTrans bumps the epoch") {
        SceneNode n({ 0, 0, 0 }, { 1, 1, 1 }, { 0, 0, 0 });
        n.UpdateTrans();
        uint64_t e1 = n.TransEpoch();
        n.SetTranslate(Vector3f(5.f, 0.f, 0.f)); // MarkTransDirty
        n.UpdateTrans();
        CHECK(n.TransEpoch() != e1);
    }

    TEST_CASE("SetRotation / SetScale each re-dirty and bump on next UpdateTrans") {
        SceneNode n;
        n.UpdateTrans();
        uint64_t a = n.TransEpoch();
        n.SetRotation(Vector3f(0.f, 0.f, 1.f));
        n.UpdateTrans();
        uint64_t b = n.TransEpoch();
        CHECK(b != a);
        n.SetScale(Vector3f(2.f, 2.f, 2.f));
        n.UpdateTrans();
        CHECK(n.TransEpoch() != b);
    }

    TEST_CASE("a parent move bumps the child epoch after the child UpdateTrans") {
        auto parent = std::make_shared<SceneNode>(
            Vector3f(1.f, 0.f, 0.f), Vector3f(1, 1, 1), Vector3f(0, 0, 0));
        auto child = std::make_shared<SceneNode>(
            Vector3f(0, 0, 0), Vector3f(1, 1, 1), Vector3f(0, 0, 0));
        parent->AppendChild(child);
        child->UpdateTrans();
        uint64_t c1 = child->TransEpoch();
        parent->SetTranslate(Vector3f(7.f, 0.f, 0.f)); // MarkTransDirty marks child too
        child->UpdateTrans();                          // recomputes through parent
        CHECK(child->TransEpoch() != c1);
        CHECK(child->ModelTrans()(0, 3) == doctest::Approx(7.0)); // value still correct
    }

    TEST_CASE("SetWorldTransform bumps the epoch (proxy-node path bypasses UpdateTrans)") {
        SceneNode n;
        n.UpdateTrans();
        uint64_t e1 = n.TransEpoch();
        n.SetWorldTransform(Matrix4d::Identity() * 3.0);
        CHECK(n.TransEpoch() != e1);
    }
} // TEST_SUITE SceneNode transform epoch

TEST_SUITE("SceneCamera VP epoch") {
    TEST_CASE("SetDirectLookAt bumps the VP epoch") {
        SceneCamera cam(16.f / 9.f, 0.1f, 100.f, 45.f); // perspective
        uint64_t    e0 = cam.VpEpoch();
        cam.SetDirectLookAt(Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));
        CHECK(cam.VpEpoch() != e0);
    }

    TEST_CASE("each Update / camera-path AdvanceTime bumps the VP epoch") {
        SceneCamera cam(16.f / 9.f, 0.1f, 100.f, 45.f);
        cam.SetDirectLookAt(Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0));
        uint64_t a = cam.VpEpoch();
        cam.Update(); // explicit recompute
        CHECK(cam.VpEpoch() != a);

        // Camera path: AdvanceTime -> SetDirectLookAt -> Update -> bump.
        CameraPath path;
        path.duration = 1.0;
        path.keyframes.push_back({ Vector3d(0, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0), 0.0 });
        path.keyframes.push_back({ Vector3d(1, 0, 5), Vector3d(0, 0, 0), Vector3d(0, 1, 0), 1.0 });
        cam.LoadPaths({ path });
        uint64_t b = cam.VpEpoch();
        cam.AdvanceTime(0.25);
        CHECK(cam.VpEpoch() != b); // animating camera bumps every frame
    }
} // TEST_SUITE SceneCamera VP epoch

// The cache stores ShaderValues; it must NEVER transform them.  This pins the
// byte-identical-store invariant the gated re-upload relies on.
TEST_SUITE("Cached matrix ShaderValue store is byte-identical") {
    TEST_CASE("fromMatrix store equals a fresh fromMatrix of the same matrix") {
        Matrix4d m = Matrix4d::Identity();
        m(0, 3)    = 12.0;
        m(1, 3)    = -3.5;
        ShaderValue stored = ShaderValue::fromMatrix(m); // what the cache holds
        ShaderValue fresh  = ShaderValue::fromMatrix(m); // a recompute on the dirty path
        REQUIRE(stored.size() == fresh.size());
        REQUIRE(stored.size() == 16u);
        for (size_t i = 0; i < stored.size(); ++i)
            CHECK(stored.data()[i] == fresh.data()[i]); // exact bits, no epsilon
    }

    TEST_CASE("inverse store equals a fresh inverse of the same matrix") {
        Matrix4d m = Matrix4d::Identity();
        m(0, 0)    = 2.0;
        m(1, 1)    = 4.0;
        ShaderValue stored = ShaderValue::fromMatrix(m.inverse());
        ShaderValue fresh  = ShaderValue::fromMatrix(m.inverse());
        for (size_t i = 0; i < 16; ++i) CHECK(stored.data()[i] == fresh.data()[i]);
    }
} // TEST_SUITE Cached matrix ShaderValue store is byte-identical
