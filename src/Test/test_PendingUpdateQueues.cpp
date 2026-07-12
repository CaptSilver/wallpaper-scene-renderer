#include <doctest.h>

#include "Scene/PendingUpdateQueues.hpp"

#include <string>

using namespace wallpaper;

// PendingUpdateQueues is the render-thread bridge's pending-update state lifted
// out of RenderHandler (SceneWallpaper.cpp) so its enqueue/merge/drain semantics
// are testable without a Vulkan link.  The setters merge per-tick exactly as the
// live queue did (last-write-wins per key, per-field text-style merge, vectors
// append in order); the withXLocked() drains hand the caller mutable refs under
// the group mutex so the render thread keeps holding the lock across the apply.
TEST_SUITE("PendingUpdateQueues") {
    TEST_CASE("setNodeTransform: origin then scale keep separate keys; per-key LWW") {
        PendingUpdateQueues q;
        q.setNodeTransform(5, "origin", 1.f, 2.f, 3.f);
        q.setNodeTransform(5, "scale", 4.f, 5.f, 6.f);
        // A second origin write for the same id wins over the first.
        q.setNodeTransform(5, "origin", 7.f, 8.f, 9.f);

        q.withPropertyLocked([&](PendingUpdateQueues& p) {
            CHECK(p.m_pending_transform_updates.size() == 2);
            auto origin = p.m_pending_transform_updates.at({ 5, "origin" });
            auto scale  = p.m_pending_transform_updates.at({ 5, "scale" });
            CHECK(origin == std::array<float, 3> { 7.f, 8.f, 9.f });
            CHECK(scale == std::array<float, 3> { 4.f, 5.f, 6.f });
            p.m_pending_transform_updates.clear();
        });
    }

    TEST_CASE("setTextStyle: successive per-field posts merge (halign then font)") {
        PendingUpdateQueues q;
        q.setTextStyle(3, "right", "", "");
        q.setTextStyle(3, "", "", "Heavy.otf");

        q.withTextLocked([&](auto& textUpdates, auto& pointsizeUpdates, auto& styleUpdates) {
            (void)textUpdates;
            (void)pointsizeUpdates;
            REQUIRE(styleUpdates.count(3) == 1);
            const auto& s = styleUpdates.at(3);
            CHECK(s.halign == "right");
            CHECK(s.valign == "");
            CHECK(s.fontName == "Heavy.otf");
        });
    }

    TEST_CASE("applyLayerBatch: F_ORIGIN|F_ALPHA unfolds origin + alpha only") {
        PendingUpdateQueues                  q;
        PendingUpdateQueues::LayerBatchEntry e {};
        e.id      = 12;
        e.flags   = PendingUpdateQueues::F_ORIGIN | PendingUpdateQueues::F_ALPHA;
        e.origin  = { 10.f, 20.f, 30.f };
        e.scale   = { 2.f, 2.f, 2.f };  // present but flag unset -> ignored
        e.angles  = { 90.f, 0.f, 0.f }; // present but flag unset -> ignored
        e.alpha   = 0.5f;
        e.visible = 1; // flag unset -> ignored
        q.applyLayerBatch({ e });

        q.withPropertyLocked([&](PendingUpdateQueues& p) {
            // origin present, scale/angles absent
            CHECK(p.m_pending_transform_updates.count({ 12, "origin" }) == 1);
            CHECK(p.m_pending_transform_updates.count({ 12, "scale" }) == 0);
            CHECK(p.m_pending_transform_updates.count({ 12, "angles" }) == 0);
            CHECK(p.m_pending_transform_updates.at({ 12, "origin" }) ==
                  std::array<float, 3> { 10.f, 20.f, 30.f });
            // alpha present, visible absent
            REQUIRE(p.m_pending_alpha_updates.count(12) == 1);
            CHECK(p.m_pending_alpha_updates.at(12) == 0.5f);
            CHECK(p.m_pending_visible_updates.count(12) == 0);
        });
    }

    TEST_CASE("scene optional: setClearColor sets once, drain resets, second drain empty") {
        PendingUpdateQueues q;
        q.setClearColor(0.1f, 0.2f, 0.3f);

        q.withPropertyLocked([&](PendingUpdateQueues& p) {
            REQUIRE(p.m_pending_clear_color.has_value());
            CHECK(*p.m_pending_clear_color == std::array<float, 3> { 0.1f, 0.2f, 0.3f });
            p.m_pending_clear_color.reset(); // mirror the drain's reset()
        });
        q.withPropertyLocked([&](PendingUpdateQueues& p) {
            CHECK_FALSE(p.m_pending_clear_color.has_value());
        });
    }

    TEST_CASE("light vectors: two setLightColor append in call order") {
        PendingUpdateQueues q;
        q.setLightColor(0, 1.f, 0.f, 0.f);
        q.setLightColor(2, 0.f, 1.f, 0.f);

        q.withPropertyLocked([&](PendingUpdateQueues& p) {
            REQUIRE(p.m_pending_light_colors.size() == 2);
            CHECK(p.m_pending_light_colors[0].index == 0);
            CHECK(p.m_pending_light_colors[0].color == std::array<float, 3> { 1.f, 0.f, 0.f });
            CHECK(p.m_pending_light_colors[1].index == 2);
            CHECK(p.m_pending_light_colors[1].color == std::array<float, 3> { 0.f, 1.f, 0.f });
        });
    }

    TEST_CASE("withColorLocked drain empties the color map (clear semantics)") {
        PendingUpdateQueues q;
        q.setColorUpdate(7, 0.5f, 0.6f, 0.7f);
        q.withColorLocked([&](auto& colorUpdates) {
            REQUIRE(colorUpdates.count(7) == 1);
            CHECK(colorUpdates.at(7) == std::array<float, 3> { 0.5f, 0.6f, 0.7f });
            colorUpdates.clear();
        });
        q.withColorLocked([&](auto& colorUpdates) {
            CHECK(colorUpdates.empty());
        });
    }
}
