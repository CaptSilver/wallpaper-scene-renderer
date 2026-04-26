#include <doctest.h>

#include "wpscene/WPParticleObject.h"

#include <nlohmann/json.hpp>

using njson = nlohmann::json;
using wallpaper::wpscene::ParticleInstanceoverride;

TEST_SUITE("ParticleInstanceoverride") {
    TEST_CASE("scalar multipliers populate from JSON") {
        njson j = {
            { "alpha", 0.5f },       { "brightness", 2.0f }, { "count", 1.4f },
            { "lifetime", 0.75f },   { "rate", 0.6f },       { "size", 0.8f },
            { "speed", 1.2f },
        };
        ParticleInstanceoverride over;
        REQUIRE(over.FromJosn(j));
        CHECK(over.enabled);
        CHECK(over.alpha == doctest::Approx(0.5f));
        CHECK(over.brightness == doctest::Approx(2.0f));
        CHECK(over.count == doctest::Approx(1.4f));
        CHECK(over.lifetime == doctest::Approx(0.75f));
        CHECK(over.rate == doctest::Approx(0.6f));
        CHECK(over.size == doctest::Approx(0.8f));
        CHECK(over.speed == doctest::Approx(1.2f));
    }

    TEST_CASE("controlpoint offset override lights up active flag") {
        njson j;
        j["controlpoint1"] = "1350.66699 1610.13318 0.00000";
        ParticleInstanceoverride over;
        REQUIRE(over.FromJosn(j));
        CHECK(over.controlpointOverrides[0].active == false);
        CHECK(over.controlpointOverrides[1].active == true);
        CHECK(over.controlpointOverrides[1].offset[0] == doctest::Approx(1350.667f));
        CHECK(over.controlpointOverrides[1].offset[1] == doctest::Approx(1610.133f));
        CHECK(over.controlpointOverrides[1].offset[2] == doctest::Approx(0.0f));
        // angle override stays untouched
        CHECK(over.controlpointOverrides[1].anglesActive == false);
        CHECK(over.controlpointOverrides[1].angles[0] == doctest::Approx(0.0f));
    }

    TEST_CASE("controlpointangle override captured independently of offset") {
        // Mirrors NieR 2B obj 113 (Молния): cp1 offset + 0.658 rad Z rotation.
        njson j;
        j["controlpoint1"]      = "1350.66699 1610.13318 0.00000";
        j["controlpointangle1"] = "0.00000 -0.00000 0.65758";
        ParticleInstanceoverride over;
        REQUIRE(over.FromJosn(j));
        CHECK(over.controlpointOverrides[1].active == true);
        CHECK(over.controlpointOverrides[1].anglesActive == true);
        CHECK(over.controlpointOverrides[1].angles[0] == doctest::Approx(0.0f));
        CHECK(over.controlpointOverrides[1].angles[1] == doctest::Approx(0.0f));
        CHECK(over.controlpointOverrides[1].angles[2] == doctest::Approx(0.65758f));
    }

    TEST_CASE("controlpointangle alone (no offset) still activates angle slot") {
        // WE's editor can author a frame-only override: CP keeps its preset offset but
        // rotates.  The plumbing must treat offset.active and angles.active as orthogonal.
        njson j;
        j["controlpointangle3"] = "0.12345 0.0 0.0";
        ParticleInstanceoverride over;
        REQUIRE(over.FromJosn(j));
        CHECK(over.controlpointOverrides[3].active == false);
        CHECK(over.controlpointOverrides[3].anglesActive == true);
        CHECK(over.controlpointOverrides[3].angles[0] == doctest::Approx(0.12345f));
    }

    TEST_CASE("CPs without any override stay inert") {
        ParticleInstanceoverride over;
        REQUIRE(over.FromJosn(njson::object()));
        for (const auto& cp : over.controlpointOverrides) {
            CHECK(cp.active == false);
            CHECK(cp.anglesActive == false);
        }
    }

    TEST_CASE("absent lifetime defaults to 0 (no-override sentinel)") {
        // Regression: `lifetime` previously defaulted to 1.0, so any JSON with
        // an `instanceoverride` block — even authoring only `colorn` /
        // `brightness` — would flip `enabled=true` and the override-init
        // function would unconditionally `SetInitLifeTime(p, over.lifetime)`,
        // stomping every authored `lifetimerandom` range to "1 second".
        // ApplyLifetimeOverride short-circuits on `<= 0.0`, so default 0.0 is
        // the correct sentinel for absent.
        njson j;
        j["colorn"]     = "1.0 0.0 0.0";
        j["brightness"] = 2.0f;
        ParticleInstanceoverride over;
        REQUIRE(over.FromJosn(j));
        CHECK(over.enabled);
        CHECK(over.lifetime == doctest::Approx(0.0f));
    }

    TEST_CASE("explicit lifetime: 0.9 still parses verbatim (NieR 2B halo)") {
        // Driver case for the "absolute seconds on sprite/halo subsystems"
        // semantic — the regression-default fix must not affect explicitly
        // authored values.
        njson j;
        j["lifetime"] = 0.9f;
        ParticleInstanceoverride over;
        REQUIRE(over.FromJosn(j));
        CHECK(over.enabled);
        CHECK(over.lifetime == doctest::Approx(0.9f));
    }

    TEST_CASE("color vs colorn are mutually exclusive") {
        {
            njson j;
            j["colorn"] = "0.30980 0.81569 1.00000";
            ParticleInstanceoverride over;
            REQUIRE(over.FromJosn(j));
            CHECK(over.overColorn == true);
            CHECK(over.overColor == false);
            CHECK(over.colorn[0] == doctest::Approx(0.30980f));
        }
        {
            njson j;
            j["color"] = "1.0 0.0 0.0";
            ParticleInstanceoverride over;
            REQUIRE(over.FromJosn(j));
            CHECK(over.overColor == true);
            CHECK(over.overColorn == false);
        }
    }
}
