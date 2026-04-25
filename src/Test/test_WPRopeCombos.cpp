#include <doctest.h>

#include "WPRopeCombos.hpp"

using namespace wallpaper;

TEST_SUITE("WPRopeCombos") {

    TEST_CASE("RendererSetsTrailRendererCombo — WE binary dispatch policy") {
        // Mirrors wallpaper64.exe FUN_1401975d0 renderer-kind switch.
        // Bug fix anchor: NieR 2B (3633635618) Разряд/Молния lightning was
        // invisible because the plain-rope path wrongly took the TRAILRENDERER
        // VS branch.  See memory/discharge-particles.md and
        // memory/wek-static-analysis.md for the dispatch table.

        // Plain rope (kind=3) — VS uses non-trail #else branch.  No TRAILRENDERER.
        CHECK_FALSE(RendererSetsTrailRendererCombo("rope"));

        // Plain sprite (kind=1) — point-sprite GS path.  No TRAILRENDERER.
        CHECK_FALSE(RendererSetsTrailRendererCombo("sprite"));

        // Spritetrail (kind=2) — moving point + history buffer.  TRAILRENDERER set.
        CHECK(RendererSetsTrailRendererCombo("spritetrail"));

        // Ropetrail (kind=4) — rope-along-history-trail.  TRAILRENDERER set.
        CHECK(RendererSetsTrailRendererCombo("ropetrail"));
    }

    TEST_CASE("RendererSetsTrailRendererCombo — defensive defaults") {
        // Unknown renderer name: don't set TRAILRENDERER (the safer default —
        // the non-trail branch handles a static-particle layout).
        CHECK_FALSE(RendererSetsTrailRendererCombo(""));
        CHECK_FALSE(RendererSetsTrailRendererCombo("unknown"));
        CHECK_FALSE(RendererSetsTrailRendererCombo("ROPE"));        // case-sensitive
        CHECK_FALSE(RendererSetsTrailRendererCombo("ropetrail "));  // trailing space
    }

} // TEST_SUITE
