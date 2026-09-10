#include <doctest.h>

#include "Scene/Scene.h"
#include "Scene/SceneCamera.h"
#include "Scene/SceneMesh.h"
#include "Scene/SceneNode.h"

#include <cmath>
#include <memory>
#include <utility>

namespace wallpaper
{
// Forward declaration of the file-local proxy-mesh generator that lives at
// namespace scope in WPSceneParser.cpp.  Exposed (non-anonymous namespace) so
// the doctest below can drive it directly without standing up the full parser
// + scene-build pipeline that InitContext requires.
void GenVolumeSphereMesh(SceneMesh& mesh);
} // namespace wallpaper

using namespace wallpaper;

TEST_SUITE("Scene") {
    TEST_CASE("PassFrameTime accumulates elapsingTime and sets frameTime") {
        Scene s;
        CHECK(s.frameTime == doctest::Approx(0.0));
        CHECK(s.elapsingTime == doctest::Approx(0.0));
        s.PassFrameTime(0.016);
        CHECK(s.frameTime == doctest::Approx(0.016));
        CHECK(s.elapsingTime == doctest::Approx(0.016));
        s.PassFrameTime(0.032);
        CHECK(s.frameTime == doctest::Approx(0.032));
        CHECK(s.elapsingTime == doctest::Approx(0.048));
    }

    TEST_CASE("UpdateLinkedCamera with no-match name is a no-op") {
        Scene s;
        s.UpdateLinkedCamera("missing"); // neither linkedCameras nor cameras populated
        // No-op contract: maps stay empty + no crash.
        CHECK(s.linkedCameras.empty());
        CHECK(s.cameras.empty());
    }

    TEST_CASE("UpdateLinkedCamera clones the source camera's width/height to followers") {
        Scene s;

        auto src          = std::make_shared<SceneCamera>(1920, 1080, 0.1f, 100.0f);
        s.cameras["main"] = src;

        auto f1                 = std::make_shared<SceneCamera>(640, 480, 0.1f, 100.0f);
        auto f2                 = std::make_shared<SceneCamera>(320, 240, 0.1f, 100.0f);
        s.cameras["follower1"]  = f1;
        s.cameras["follower2"]  = f2;
        s.linkedCameras["main"] = { "follower1", "follower2", "missing_follower" };

        s.UpdateLinkedCamera("main");
        // Clone() copies width/height; followers now match the source.
        CHECK(f1->Width() == doctest::Approx(1920.0));
        CHECK(f1->Height() == doctest::Approx(1080.0));
        CHECK(f2->Width() == doctest::Approx(1920.0));
        CHECK(f2->Height() == doctest::Approx(1080.0));
    }

    TEST_CASE("VolumetricsConfig defaults: disabled, density multiplier 1.0") {
        Scene s;
        CHECK(s.volumetricsConfig.enabled == false);
        CHECK(s.volumetricsConfig.globalDensityMultiplier == doctest::Approx(1.0f));
    }

    TEST_CASE("Scene::volumetricLights filters lights by isVolumetricEmitterCandidate "
              "predicate (castsVolumetrics + kind)") {
        Scene s;
        // Light 0: density=0 — should NOT appear in the filtered list.
        s.lights.emplace_back(std::make_unique<SceneLight>(
            Eigen::Vector3f(1, 1, 1), 100.0f, 1.0f));
        // Light 1: density=5 — heuristic opts in.
        {
            auto l = std::make_unique<SceneLight>(
                Eigen::Vector3f(1, 1, 1), 100.0f, 1.0f);
            SceneLight::VolumetricParams vp;
            vp.density = 5.0f;
            l->setVolumetric(vp);
            s.lights.emplace_back(std::move(l));
        }
        // Light 2: density=2 (editor default) — heuristic opts in.
        {
            auto l = std::make_unique<SceneLight>(
                Eigen::Vector3f(1, 1, 1), 100.0f, 1.0f);
            SceneLight::VolumetricParams vp;
            vp.density = 2.0f;
            l->setVolumetric(vp);
            s.lights.emplace_back(std::move(l));
        }
        // Light 3: explicit:false with density>0 — must be filtered out.
        {
            auto l = std::make_unique<SceneLight>(
                Eigen::Vector3f(1, 1, 1), 100.0f, 1.0f);
            SceneLight::VolumetricParams vp;
            vp.cast_volumetrics_explicit = true;
            vp.cast_volumetrics_value    = false;
            vp.density                   = 5.0f;
            l->setVolumetric(vp);
            s.lights.emplace_back(std::move(l));
        }
        auto vols = s.volumetricLights();
        REQUIRE(vols.size() == 2);
        // Pointers preserved in original order.
        CHECK(vols[0] == s.lights[1].get());
        CHECK(vols[1] == s.lights[2].get());
    }

    TEST_CASE("Scene::volumetricLights returns empty when no light casts") {
        Scene s;
        s.lights.emplace_back(std::make_unique<SceneLight>(
            Eigen::Vector3f(1, 1, 1), 100.0f, 1.0f));
        auto vols = s.volumetricLights();
        CHECK(vols.empty());
    }

    TEST_CASE("Scene::volumetricLights filters non-Point/LPoint kinds") {
        // Predicate intentionally matches the per-frame uniform writer: the
        // chain only emits Point/LPoint today, so an LSpot/LTube/LDirectional
        // light authoring volumetric fields must NOT appear in the accessor
        // (else a future caller would route uniforms to a no-op pass).
        Scene s;
        auto mkLight = [](SceneLight::LightKind k) {
            auto l = std::make_unique<SceneLight>(
                Eigen::Vector3f(1, 1, 1), 100.0f, 1.0f);
            SceneLight::VolumetricParams vp;
            vp.density = 5.0f;
            l->setVolumetric(vp);
            l->setKind(k);
            return l;
        };
        s.lights.emplace_back(mkLight(SceneLight::LightKind::Point));
        s.lights.emplace_back(mkLight(SceneLight::LightKind::LSpot));
        s.lights.emplace_back(mkLight(SceneLight::LightKind::LPoint));
        s.lights.emplace_back(mkLight(SceneLight::LightKind::LTube));
        s.lights.emplace_back(mkLight(SceneLight::LightKind::LDirectional));
        auto vols = s.volumetricLights();
        REQUIRE(vols.size() == 2);
        CHECK(vols[0] == s.lights[0].get()); // Point
        CHECK(vols[1] == s.lights[2].get()); // LPoint
    }

} // Scene

TEST_SUITE("Scene_DefaultVolumeSphere") {
    TEST_CASE("default_volume_sphere is empty on a bare Scene (populated by InitContext)") {
        // Bare ctor leaves the field default-constructed — no vertex arrays,
        // no index arrays.  The populator runs in WPSceneParser::InitContext.
        Scene s;
        CHECK(s.default_volume_sphere.VertexCount() == 0);
        CHECK(s.default_volume_sphere.IndexCount() == 0);
    }

    TEST_CASE("GenVolumeSphereMesh produces a non-empty vertex + index array") {
        Scene s;
        GenVolumeSphereMesh(s.default_volume_sphere);
        // VertexCount / IndexCount on SceneMesh return the number of vertex
        // and index ARRAYS (not vertices); the populator should add exactly
        // one of each.
        CHECK(s.default_volume_sphere.VertexCount() == 1);
        CHECK(s.default_volume_sphere.IndexCount() == 1);
        const auto& va = s.default_volume_sphere.GetVertexArray(0);
        const auto& ia = s.default_volume_sphere.GetIndexArray(0);
        // Icosahedron: 12 vertices, 20 triangles.
        CHECK(va.VertexCount() == 12u);
        CHECK(ia.DataCount() == 60u);
    }

    // This mesh writes full 32-bit index values, unlike every other producer,
    // which packs two 16-bit indices per slot.  Bound as UINT16 it draws 40
    // triangles built from index halves instead of 20 real ones.
    TEST_CASE("the volume sphere declares 32-bit indices") {
        Scene s;
        GenVolumeSphereMesh(s.default_volume_sphere);
        const auto& ia = s.default_volume_sphere.GetIndexArray(0);
        CHECK(ia.Width() == SceneIndexArray::IndexWidth::U32);
        CHECK(ia.IndexElemCount() == 60u);
        CHECK(ia.DrawIndexCount() == 60u);
    }
}

TEST_SUITE("SceneIndexArray") {
    // The draw call needs a whole number of triangles.  A 16-bit array's slot
    // count times two can leave a trailing index that completes no triangle —
    // the odd-triangle padding slot from the packing helper.
    TEST_CASE("draw count rounds down to whole triangles") {
        const std::array<uint32_t, 3> tri { 0, 1, 2 };

        SceneIndexArray wide(std::span<const uint32_t> { tri.data(), tri.size() },
                             SceneIndexArray::IndexWidth::U32);
        CHECK(wide.IndexElemCount() == 3u);
        CHECK(wide.DrawIndexCount() == 3u);

        // Two slots hold four 16-bit indices: one triangle plus a spare.
        SceneIndexArray narrow(std::span<const uint32_t> { tri.data(), 2 },
                               SceneIndexArray::IndexWidth::U16);
        CHECK(narrow.IndexElemCount() == 4u);
        CHECK(narrow.DrawIndexCount() == 3u);
    }

    TEST_CASE("render draw count honours the render size cap") {
        std::array<uint32_t, 6> six { 0, 1, 2, 3, 4, 5 };
        SceneIndexArray         wide(std::span<const uint32_t> { six.data(), six.size() },
                             SceneIndexArray::IndexWidth::U32);
        CHECK(wide.DrawIndexCount() == 6u);

        wide.SetRenderDataCount(4);
        CHECK(wide.RenderIndexElemCount() == 4u);
        CHECK(wide.RenderDrawIndexCount() == 3u);
    }

    TEST_CASE("all icosahedron vertices lie on a unit sphere") {
        Scene s;
        GenVolumeSphereMesh(s.default_volume_sphere);
        const auto& va    = s.default_volume_sphere.GetVertexArray(0);
        const float* data = va.Data();
        REQUIRE(data != nullptr);
        REQUIRE(va.VertexCount() == 12u);
        // SceneVertexArray pads each FLOAT3 attribute to 4 floats by default
        // (RealAttributeSize: padding=true -> 4); OneSize() reflects the real
        // per-vertex stride.
        const std::size_t stride = va.OneSize();
        REQUIRE(stride >= 3u);
        for (std::size_t i = 0; i < va.VertexCount(); ++i) {
            const float x   = data[i * stride + 0];
            const float y   = data[i * stride + 1];
            const float z   = data[i * stride + 2];
            const float len = std::sqrt(x * x + y * y + z * z);
            CHECK(len == doctest::Approx(1.0f).epsilon(1e-5));
        }
    }
} // Scene_DefaultVolumeSphere

// ===========================================================================
// Pending-parent-change queue + ApplyPendingParentChanges drain
// ===========================================================================

TEST_SUITE("Scene.PendingParentChanges") {
    TEST_CASE("queue stores child→parent pairs in FIFO order") {
        Scene s;
        s.QueueParentChange(10, 20);
        s.QueueParentChange(11, -1);
        s.QueueParentChange(12, 20);
        auto pending = s.TakePendingParentChanges();
        REQUIRE(pending.size() == 3);
        CHECK(pending[0] == std::pair<i32, i32> { 10, 20 });
        CHECK(pending[1] == std::pair<i32, i32> { 11, -1 });
        CHECK(pending[2] == std::pair<i32, i32> { 12, 20 });
    }

    TEST_CASE("Take leaves the queue empty for next caller") {
        Scene s;
        s.QueueParentChange(1, 2);
        (void)s.TakePendingParentChanges();
        auto pending = s.TakePendingParentChanges();
        CHECK(pending.empty());
    }

    TEST_CASE("ApplyPendingParentChanges reparents a child to a new parent") {
        Scene s;
        auto  a   = std::make_shared<SceneNode>();
        auto  b   = std::make_shared<SceneNode>();
        auto  c   = std::make_shared<SceneNode>();
        a->ID()   = 1;
        b->ID()   = 2;
        c->ID()   = 3;
        s.sceneGraph = std::make_shared<SceneNode>();
        s.sceneGraph->AppendChild(a);
        s.sceneGraph->AppendChild(b);
        a->AppendChild(c);
        s.nodeById[1] = a.get();
        s.nodeById[2] = b.get();
        s.nodeById[3] = c.get();

        s.QueueParentChange(3, 2); // reparent c from a → b
        s.ApplyPendingParentChanges();

        CHECK(c->Parent() == b.get());
        CHECK(a->GetChildren().empty());
        CHECK(b->GetChildren().size() == 1);
        CHECK(b->GetChildren().front().get() == c.get());
    }

    TEST_CASE("ApplyPendingParentChanges with parent_id=-1 reattaches to sceneGraph") {
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto p       = std::make_shared<SceneNode>();
        auto c       = std::make_shared<SceneNode>();
        p->ID()      = 1;
        c->ID()      = 2;
        s.sceneGraph->AppendChild(p);
        p->AppendChild(c);
        s.nodeById[1] = p.get();
        s.nodeById[2] = c.get();

        s.QueueParentChange(2, -1);
        s.ApplyPendingParentChanges();

        CHECK(c->Parent() == s.sceneGraph.get());
        CHECK(p->GetChildren().empty());
    }

    TEST_CASE("ApplyPendingParentChanges skips unknown child id") {
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        s.QueueParentChange(999, -1); // unknown id
        CHECK_NOTHROW(s.ApplyPendingParentChanges());
    }

    TEST_CASE("ApplyPendingParentChanges skips unknown parent id (non-root)") {
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto c       = std::make_shared<SceneNode>();
        c->ID()      = 1;
        s.sceneGraph->AppendChild(c);
        s.nodeById[1] = c.get();

        s.QueueParentChange(1, 999);
        s.ApplyPendingParentChanges();
        // No-op: c stays under sceneGraph
        CHECK(c->Parent() == s.sceneGraph.get());
    }

} // Scene.PendingParentChanges

// ===========================================================================
// ResolveParentNodeId — cross-thread (QML) reverse lookup of a child's
// parent id.  Companion to ApplyPendingParentChanges: shares
// m_pending_parent_mutex so the parent-walk never races a reparent on the
// render thread.  Returns -1 on any miss.
// ===========================================================================

TEST_SUITE("Scene.ResolveParentNodeId") {
    TEST_CASE("resolves a child to its parent's id") {
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto p       = std::make_shared<SceneNode>();
        auto c       = std::make_shared<SceneNode>();
        p->ID()      = 7;
        c->ID()      = 8;
        s.sceneGraph->AppendChild(p);
        p->AppendChild(c);
        s.nodeById[7] = p.get();
        s.nodeById[8] = c.get();

        CHECK(s.ResolveParentNodeId(8) == 7);
    }

    TEST_CASE("reflects a reparent after ApplyPendingParentChanges drains") {
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto a       = std::make_shared<SceneNode>();
        auto b       = std::make_shared<SceneNode>();
        auto c       = std::make_shared<SceneNode>();
        a->ID()      = 1;
        b->ID()      = 2;
        c->ID()      = 3;
        s.sceneGraph->AppendChild(a);
        s.sceneGraph->AppendChild(b);
        a->AppendChild(c);
        s.nodeById[1] = a.get();
        s.nodeById[2] = b.get();
        s.nodeById[3] = c.get();

        // Before the drain, c's parent is a (id 1).
        CHECK(s.ResolveParentNodeId(3) == 1);

        s.QueueParentChange(3, 2); // reparent c from a → b
        s.ApplyPendingParentChanges();

        // After the drain, c's parent is b (id 2).
        CHECK(s.ResolveParentNodeId(3) == 2);
    }

    TEST_CASE("unknown childId returns -1") {
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto p       = std::make_shared<SceneNode>();
        p->ID()      = 1;
        s.sceneGraph->AppendChild(p);
        s.nodeById[1] = p.get();

        CHECK(s.ResolveParentNodeId(999) == -1);
    }

    TEST_CASE("root/parentless node returns -1") {
        // A node with no parent pointer (never AppendChild'd to anything).
        Scene s;
        auto orphan   = std::make_shared<SceneNode>();
        orphan->ID()  = 5;
        s.nodeById[5] = orphan.get();
        REQUIRE(orphan->Parent() == nullptr);

        CHECK(s.ResolveParentNodeId(5) == -1);
    }

    TEST_CASE("parent not present in nodeById returns -1") {
        // child has a live parent pointer, but that parent was never
        // registered in nodeById, so the reverse map walk finds no id.
        Scene s;
        auto p       = std::make_shared<SceneNode>();
        auto c       = std::make_shared<SceneNode>();
        p->ID()      = 1;
        c->ID()      = 2;
        p->AppendChild(c);
        // Only the child is registered; the parent is intentionally absent.
        s.nodeById[2] = c.get();
        REQUIRE(c->Parent() == p.get());

        CHECK(s.ResolveParentNodeId(2) == -1);
    }

} // Scene.ResolveParentNodeId

// ===========================================================================
// Pending-child-sort queue + ApplyPendingChildSorts drain
// (thisScene.sortLayer bridge — Blue Archive 2764537029 visualizer)
// ===========================================================================

TEST_SUITE("Scene.PendingChildSorts") {
    TEST_CASE("queue stores child→index pairs in FIFO order") {
        Scene s;
        s.QueueChildSort(10, 0);
        s.QueueChildSort(11, 5);
        s.QueueChildSort(12, 99);
        auto pending = s.TakePendingChildSorts();
        REQUIRE(pending.size() == 3);
        CHECK(pending[0] == std::pair<i32, i32> { 10, 0 });
        CHECK(pending[1] == std::pair<i32, i32> { 11, 5 });
        CHECK(pending[2] == std::pair<i32, i32> { 12, 99 });
    }

    TEST_CASE("Take leaves the queue empty for next caller") {
        Scene s;
        s.QueueChildSort(1, 2);
        (void)s.TakePendingChildSorts();
        auto pending = s.TakePendingChildSorts();
        CHECK(pending.empty());
    }

    TEST_CASE("ApplyPendingChildSorts moves child to target index") {
        // parent has children [a, b, c]; sort c → index 0; expect [c, a, b].
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto p   = std::make_shared<SceneNode>();
        auto a   = std::make_shared<SceneNode>();
        auto b   = std::make_shared<SceneNode>();
        auto c   = std::make_shared<SceneNode>();
        p->ID()  = 1; a->ID() = 10; b->ID() = 11; c->ID() = 12;
        s.sceneGraph->AppendChild(p);
        p->AppendChild(a);
        p->AppendChild(b);
        p->AppendChild(c);
        s.nodeById[1]  = p.get();
        s.nodeById[10] = a.get();
        s.nodeById[11] = b.get();
        s.nodeById[12] = c.get();

        s.QueueChildSort(12, 0);
        s.ApplyPendingChildSorts();

        REQUIRE(p->GetChildren().size() == 3);
        auto it = p->GetChildren().begin();
        CHECK((*it++).get() == c.get());
        CHECK((*it++).get() == a.get());
        CHECK((*it++).get() == b.get());
    }

    TEST_CASE("ApplyPendingChildSorts clamps target_index above end") {
        // children [a, b, c]; sort a → index 99; expect [b, c, a] (clamps to size-1=2).
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto p   = std::make_shared<SceneNode>();
        auto a   = std::make_shared<SceneNode>();
        auto b   = std::make_shared<SceneNode>();
        auto c   = std::make_shared<SceneNode>();
        p->ID()  = 1; a->ID() = 10; b->ID() = 11; c->ID() = 12;
        s.sceneGraph->AppendChild(p);
        p->AppendChild(a);
        p->AppendChild(b);
        p->AppendChild(c);
        s.nodeById[10] = a.get();
        s.nodeById[11] = b.get();
        s.nodeById[12] = c.get();

        s.QueueChildSort(10, 99);
        s.ApplyPendingChildSorts();

        REQUIRE(p->GetChildren().size() == 3);
        auto it = p->GetChildren().begin();
        CHECK((*it++).get() == b.get());
        CHECK((*it++).get() == c.get());
        CHECK((*it++).get() == a.get());
    }

    TEST_CASE("ApplyPendingChildSorts clamps negative target_index to 0") {
        // children [a, b]; sort b → index -5; expect [b, a].
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto p   = std::make_shared<SceneNode>();
        auto a   = std::make_shared<SceneNode>();
        auto b   = std::make_shared<SceneNode>();
        p->ID()  = 1; a->ID() = 10; b->ID() = 11;
        s.sceneGraph->AppendChild(p);
        p->AppendChild(a);
        p->AppendChild(b);
        s.nodeById[10] = a.get();
        s.nodeById[11] = b.get();

        s.QueueChildSort(11, -5);
        s.ApplyPendingChildSorts();

        REQUIRE(p->GetChildren().size() == 2);
        auto it = p->GetChildren().begin();
        CHECK((*it++).get() == b.get());
        CHECK((*it++).get() == a.get());
    }

    TEST_CASE("ApplyPendingChildSorts is a no-op for unknown child id") {
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        s.QueueChildSort(999, 0);
        CHECK_NOTHROW(s.ApplyPendingChildSorts());
    }

    TEST_CASE("ApplyPendingChildSorts is a no-op for orphan child (no parent)") {
        // Child registered in nodeById but never attached to a parent.
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto orphan  = std::make_shared<SceneNode>();
        orphan->ID() = 5;
        s.nodeById[5] = orphan.get();

        s.QueueChildSort(5, 0);
        CHECK_NOTHROW(s.ApplyPendingChildSorts());
    }

    TEST_CASE("Visualizer pattern: 64 sortLayer calls all collapse to same target") {
        // Direct repro of Blue Archive's init() loop: every spawned bar gets
        // sortLayer(bar, thisIndex).  Each call moves the bar to thisIndex,
        // pushing previous siblings down; the order is reverse-creation.
        Scene s;
        s.sceneGraph = std::make_shared<SceneNode>();
        auto p       = std::make_shared<SceneNode>();
        p->ID()      = 1;
        s.sceneGraph->AppendChild(p);
        s.nodeById[1] = p.get();

        // Build [t, b0, b1, b2, b3] where t is the template at index 0.
        const int kTemplateIndex = 0;
        auto t = std::make_shared<SceneNode>(); t->ID() = 100; p->AppendChild(t);
        s.nodeById[100] = t.get();
        std::vector<std::shared_ptr<SceneNode>> bars;
        for (int i = 0; i < 4; ++i) {
            auto b = std::make_shared<SceneNode>();
            b->ID() = 200 + i;
            p->AppendChild(b);
            s.nodeById[200 + i] = b.get();
            bars.push_back(b);
        }
        // Visualizer init() then calls sortLayer on each spawned bar to
        // pin them at the template's index.
        for (int i = 0; i < 4; ++i) {
            s.QueueChildSort(200 + i, kTemplateIndex);
        }
        s.ApplyPendingChildSorts();

        // After the drain, the four bars are at indices 0..3 (in
        // reverse-spawn order — last sort wins for that slot, earlier
        // bars shifted down) and the template is pushed to the back.
        REQUIRE(p->GetChildren().size() == 5);
        auto it = p->GetChildren().begin();
        CHECK((*it++).get() == bars[3].get());
        CHECK((*it++).get() == bars[2].get());
        CHECK((*it++).get() == bars[1].get());
        CHECK((*it++).get() == bars[0].get());
        CHECK((*it++).get() == t.get());
    }

} // Scene.PendingChildSorts

TEST_SUITE("Scene_Skybox") {
    // 360° / skybox cubemap detection-only stub.  Defaults stay off so the
    // non-skybox path remains byte-identical; consumers downstream of the
    // parser key off has_skybox and the skyboxLayerIds list.
    TEST_CASE("default-constructed Scene has has_skybox false and empty layer list") {
        Scene s;
        CHECK(s.has_skybox == false);
        CHECK(s.skyboxLayerIds.empty());
    }

    TEST_CASE("populating skyboxLayerIds + flag is independent of other Scene state") {
        Scene s;
        s.has_skybox = true;
        s.skyboxLayerIds.push_back(7);
        s.skyboxLayerIds.push_back(42);

        // Other defaults remain untouched — toggling the skybox state must not
        // shift fields a regression oracle compares against.
        CHECK(s.hdrContent == false);
        CHECK(s.has_skybox == true);
        REQUIRE(s.skyboxLayerIds.size() == 2);
        CHECK(s.skyboxLayerIds[0] == 7);
        CHECK(s.skyboxLayerIds[1] == 42);
    }

    TEST_CASE("skyboxTexKey defaults empty and records the resolved panorama key") {
        Scene s;
        CHECK(s.skyboxTexKey.empty());
        s.skyboxTexKey = "materials/pano.png";
        CHECK(s.skyboxTexKey == "materials/pano.png");
    }
}

// ===========================================================================
// Camera layer
// ===========================================================================

#include "CameraLayer.hpp"

TEST_SUITE("Scene camera layer") {
    // Wallpaper Engine keeps the runtime camera as an object in the layer list.
    // The scene-level `camera` block is the editor's saved viewport, and in
    // scenes that carry both they disagree — Real-Time Earth (3557068717) puts
    // its camera layer at z=0.35 while the saved block sits at z=0.708.
    TEST_CASE("a camera layer is found among ordinary objects") {
        auto objects = nlohmann::json::parse(R"([
            {"id": 156, "name": "GROUND", "model": "models/a.mdl"},
            {"id": 210, "name": "", "camera": "default",
             "origin": "0.00000 0.00000 0.35000", "fov": 50.0, "zoom": 1.0},
            {"id": 225, "name": "clock", "text": {"value": "12:00"}}
        ])");

        auto layer = wallpaper::FindCameraLayer(objects);
        REQUIRE(layer.found);
        CHECK(layer.origin[2] == doctest::Approx(0.35f));
        CHECK(layer.fov == doctest::Approx(50.0f));
    }

    TEST_CASE("a scene with no camera layer reports none") {
        auto objects = nlohmann::json::parse(R"([
            {"id": 1, "image": "materials/a.json"}
        ])");
        CHECK_FALSE(wallpaper::FindCameraLayer(objects).found);
    }

    // A camera layer without `angles` means identity orientation — looking
    // down -Z.  Inheriting the saved block's `center` instead would tilt the
    // view off the subject.
    TEST_CASE("a camera layer without angles looks down negative Z") {
        wallpaper::SceneCameraLayer layer;
        layer.found  = true;
        layer.origin = { 0.0f, 0.0f, 0.35f };

        auto basis = wallpaper::CameraBasisFromLayer(layer);
        CHECK(basis.eye[2] == doctest::Approx(0.35));
        CHECK(basis.center[0] == doctest::Approx(0.0));
        CHECK(basis.center[1] == doctest::Approx(0.0));
        CHECK(basis.center[2] == doctest::Approx(-0.65)); // 0.35 - 1
        CHECK(basis.up[1] == doctest::Approx(1.0));
    }

    TEST_CASE("a yaw of ninety degrees points the camera down negative X") {
        wallpaper::SceneCameraLayer layer;
        layer.found  = true;
        layer.angles = { 0.0f, 1.57079633f, 0.0f };

        auto basis = wallpaper::CameraBasisFromLayer(layer);
        CHECK(basis.center[0] == doctest::Approx(-1.0).epsilon(0.001));
        CHECK(basis.center[2] == doctest::Approx(0.0).epsilon(0.001));
        CHECK(basis.up[1] == doctest::Approx(1.0));
    }
}


TEST_SUITE("Scene camera layer fov") {
    TEST_CASE("a camera layer's fov wins over the scene block") {
        wallpaper::SceneCameraLayer layer;
        layer.found = true;
        layer.fov   = 50.0f;
        CHECK(wallpaper::CameraFovFor(layer, 34.0f) == doctest::Approx(50.0f));
    }

    TEST_CASE("a camera layer without an fov falls back to the scene block") {
        wallpaper::SceneCameraLayer layer;
        layer.found = true;
        layer.fov   = 0.0f;
        CHECK(wallpaper::CameraFovFor(layer, 34.0f) == doctest::Approx(34.0f));
    }

    TEST_CASE("no camera layer means the scene block's fov") {
        CHECK(wallpaper::CameraFovFor(wallpaper::SceneCameraLayer {}, 34.0f) ==
              doctest::Approx(34.0f));
    }
}

// ===========================================================================
// Normal matrix
// ===========================================================================

#include "NormalMatrix.hpp"

TEST_SUITE("Normal matrix") {
    // A uniform scale leaves normal directions alone once renormalized, but a
    // non-uniform one does not: stretching X by 2 must compress the normal's X
    // by the same factor or lit surfaces shade as if unscaled.
    TEST_CASE("a non-uniform scale inverts on each axis") {
        Eigen::Matrix4d model = Eigen::Matrix4d::Identity();
        model(0, 0)           = 2.0;
        model(1, 1)           = 1.0;
        model(2, 2)           = 4.0;

        auto n = wallpaper::NormalMatrixFrom(model);
        CHECK(n(0, 0) == doctest::Approx(0.5));
        CHECK(n(1, 1) == doctest::Approx(1.0));
        CHECK(n(2, 2) == doctest::Approx(0.25));
    }

    // Under rotation the inverse transpose is the rotation itself.  This is the
    // case that matters most: leaving it at identity leaves normals in object
    // space while light positions are uploaded in world space, so the lit
    // hemisphere ends up wherever the model's own rotation happens to put it.
    TEST_CASE("a rotation passes through unchanged") {
        Eigen::Matrix4d model = Eigen::Matrix4d::Identity();
        model.topLeftCorner<3, 3>() =
            Eigen::AngleAxisd(1.0, Eigen::Vector3d::UnitY()).toRotationMatrix();

        auto n = wallpaper::NormalMatrixFrom(model);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) CHECK(n(r, c) == doctest::Approx(model(r, c)));
    }

    TEST_CASE("translation does not reach the normal matrix") {
        Eigen::Matrix4d model = Eigen::Matrix4d::Identity();
        model(0, 3)           = 2346.0;

        auto n = wallpaper::NormalMatrixFrom(model);
        CHECK(n(0, 3) == doctest::Approx(0.0));
        CHECK(n(0, 0) == doctest::Approx(1.0));
    }

    // A zero scale on an axis has no inverse; the upload must stay finite.
    TEST_CASE("a degenerate scale does not produce NaN") {
        Eigen::Matrix4d model = Eigen::Matrix4d::Identity();
        model(1, 1)           = 0.0;

        auto n = wallpaper::NormalMatrixFrom(model);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) CHECK(std::isfinite(n(r, c)));
    }
}

// ===========================================================================
// Authored parent chain
// ===========================================================================

#include "AuthoredChain.hpp"

TEST_SUITE("Authored parent chain") {
    using Chain = std::unordered_map<wallpaper::i32, std::pair<wallpaper::i32, Eigen::Matrix4d>>;

    static Eigen::Matrix4d translation(double x) {
        Eigen::Matrix4d m = Eigen::Matrix4d::Identity();
        m(0, 3)           = x;
        return m;
    }

    TEST_CASE("a child accumulates its ancestors' transforms") {
        Chain c;
        c[10] = { -1, translation(100.0) };
        c[20] = { 10, translation(5.0) };
        CHECK(wallpaper::WorldFromAuthoredChain(20, c)(0, 3) == doctest::Approx(105.0));
    }

    TEST_CASE("a root object keeps its own transform") {
        Chain c;
        c[10] = { -1, translation(100.0) };
        CHECK(wallpaper::WorldFromAuthoredChain(10, c)(0, 3) == doctest::Approx(100.0));
    }

    TEST_CASE("an unknown id contributes identity") {
        Chain c;
        CHECK(wallpaper::WorldFromAuthoredChain(99, c).isIdentity());
    }

    // A malformed scene must not hang the parser.
    TEST_CASE("a parent cycle terminates") {
        Chain c;
        c[1] = { 2, translation(1.0) };
        c[2] = { 1, translation(1.0) };
        auto w = wallpaper::WorldFromAuthoredChain(1, c);
        CHECK(std::isfinite(w(0, 3)));
    }
}
