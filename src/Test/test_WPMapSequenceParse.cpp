#include <doctest.h>

#include "WPMapSequenceParse.hpp"

#include <nlohmann/json.hpp>

using namespace wallpaper;
using nlohmann::json;

TEST_SUITE("WPMapSequenceParse — limitbehavior") {
    TEST_CASE("\"mirror\" → mirror=true") {
        CHECK(LimitBehaviorIsMirror("mirror"));
    }
    TEST_CASE("\"repeat\" → mirror=false") {
        CHECK_FALSE(LimitBehaviorIsMirror("repeat"));
    }
    TEST_CASE("absent (empty string) → mirror=false") {
        CHECK_FALSE(LimitBehaviorIsMirror(""));
    }
    TEST_CASE("\"clamp\" is not a real WE mode → mirror=false") {
        CHECK_FALSE(LimitBehaviorIsMirror("clamp"));
    }
    TEST_CASE("unknown string → mirror=false") {
        CHECK_FALSE(LimitBehaviorIsMirror("bogus"));
    }
}

TEST_SUITE("WPMapSequenceParse — ParseBetweenParams") {
    TEST_CASE("defaults: cpStart=0, cpEnd=1, count=10, no arc, no size reduction") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":10})");
        auto p = ParseBetweenParams(j);
        CHECK(p.cp_start == 0);
        CHECK(p.cp_end == 1);
        CHECK(p.count == 10u);
        CHECK(p.arc_amount == doctest::Approx(0.0f));
        CHECK(p.size_reduction_amount == doctest::Approx(0.0f));
        CHECK(p.arc_direction[0] == doctest::Approx(0.0f));
        CHECK(p.arc_direction[1] == doctest::Approx(0.0f));
        CHECK(p.arc_direction[2] == doctest::Approx(0.0f));
        CHECK_FALSE(p.mirror);
    }

    TEST_CASE("absent count defaults to 1") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints"})");
        auto p = ParseBetweenParams(j);
        CHECK(p.count == 1u);
    }

    TEST_CASE("count=0 is treated as 1 (avoids div-by-zero)") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":0})");
        auto p = ParseBetweenParams(j);
        CHECK(p.count == 1u);
    }

    TEST_CASE("controlpointstart honored") {
        json j =
            json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":5,"controlpointstart":3})");
        auto p = ParseBetweenParams(j);
        CHECK(p.cp_start == 3);
        CHECK(p.cp_end == 1);
    }

    TEST_CASE("controlpointend honored") {
        json j =
            json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":5,"controlpointend":4})");
        auto p = ParseBetweenParams(j);
        CHECK(p.cp_start == 0);
        CHECK(p.cp_end == 4);
    }

    TEST_CASE("controlpointstart/end clamp to 7") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":5,
                                  "controlpointstart":99,"controlpointend":42})");
        auto p = ParseBetweenParams(j);
        CHECK(p.cp_start == 7);
        CHECK(p.cp_end == 7);
    }

    TEST_CASE("negative controlpointstart/end clamp to 0") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":5,
                                  "controlpointstart":-5,"controlpointend":-3})");
        auto p = ParseBetweenParams(j);
        CHECK(p.cp_start == 0);
        CHECK(p.cp_end == 0);
    }

    TEST_CASE("limitbehavior=\"mirror\" sets mirror flag") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":5,
                                  "limitbehavior":"mirror"})");
        auto p = ParseBetweenParams(j);
        CHECK(p.mirror);
    }

    TEST_CASE("limitbehavior=\"repeat\" leaves mirror false") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":5,
                                  "limitbehavior":"repeat"})");
        auto p = ParseBetweenParams(j);
        CHECK_FALSE(p.mirror);
    }

    TEST_CASE("flags bit 0x02 also forces mirror (NieR thunderbolt empirical bit)") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":5,
                                  "flags":2})");
        auto p = ParseBetweenParams(j);
        CHECK(p.mirror);
    }

    TEST_CASE("arcamount field stored verbatim") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":5,
                                  "arcamount":0.75})");
        auto p = ParseBetweenParams(j);
        CHECK(p.arc_amount == doctest::Approx(0.75f));
    }

    TEST_CASE("arcdirection field parsed as vec3 string") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":5,
                                  "arcdirection":"0.0 0.0 1.0"})");
        auto p = ParseBetweenParams(j);
        CHECK(p.arc_direction[0] == doctest::Approx(0.0f));
        CHECK(p.arc_direction[1] == doctest::Approx(0.0f));
        CHECK(p.arc_direction[2] == doctest::Approx(1.0f));
    }

    TEST_CASE("sizereductionamount field stored verbatim") {
        json j = json::parse(R"({"name":"mapsequencebetweencontrolpoints","count":5,
                                  "sizereductionamount":0.5})");
        auto p = ParseBetweenParams(j);
        CHECK(p.size_reduction_amount == doctest::Approx(0.5f));
    }
}

TEST_SUITE("WPMapSequenceParse — ParseAroundParams") {
    TEST_CASE("defaults") {
        json j = json::parse(R"({"name":"mapsequencearoundcontrolpoint","count":7})");
        auto p = ParseAroundParams(j);
        CHECK(p.cp == 0);
        CHECK(p.count == 7u);
        CHECK_FALSE(p.mirror);
    }

    TEST_CASE("count=0 → 1") {
        json j = json::parse(R"({"name":"mapsequencearoundcontrolpoint","count":0})");
        auto p = ParseAroundParams(j);
        CHECK(p.count == 1u);
    }

    TEST_CASE("controlpoint clamp to 7") {
        json j = json::parse(R"({"name":"mapsequencearoundcontrolpoint","count":5,
                                  "controlpoint":12})");
        auto p = ParseAroundParams(j);
        CHECK(p.cp == 7);
    }

    TEST_CASE("limitbehavior=\"mirror\"") {
        json j = json::parse(R"({"name":"mapsequencearoundcontrolpoint","count":5,
                                  "limitbehavior":"mirror"})");
        auto p = ParseAroundParams(j);
        CHECK(p.mirror);
    }
}

TEST_SUITE("WPMapSequenceParse — CollectCpReferences") {
    TEST_CASE("returns empty for an op with no CP fields") {
        json j   = json::parse(R"({"name":"vortex"})");
        auto refs = CollectCpReferences(j);
        CHECK(refs.empty());
    }

    TEST_CASE("singular controlpoint field is collected") {
        json j   = json::parse(R"({"name":"controlpointforce","controlpoint":3})");
        auto refs = CollectCpReferences(j);
        REQUIRE(refs.size() == 1);
        CHECK(refs[0] == 3);
    }

    TEST_CASE("controlpointstart + controlpointend both collected") {
        json j   = json::parse(R"({"name":"mapsequencebetweencontrolpoints",
                                     "controlpointstart":1,"controlpointend":4})");
        auto refs = CollectCpReferences(j);
        CHECK(refs.size() == 2);
        // Order is start, end per the parser walk
        CHECK(refs[0] == 1);
        CHECK(refs[1] == 4);
    }

    TEST_CASE("inputcontrolpoint0/1 (remapinitialvalue / distancetocontrolpoint) collected") {
        json j   = json::parse(R"({"name":"remapinitialvalue","input":"distancetocontrolpoint",
                                     "inputcontrolpoint0":2,"inputcontrolpoint1":5})");
        auto refs = CollectCpReferences(j);
        CHECK(refs.size() == 2);
        CHECK(refs[0] == 2);
        CHECK(refs[1] == 5);
    }

    TEST_CASE("clamp >7 to 7 — out-of-range still counts as a reference") {
        json j   = json::parse(R"({"name":"controlpointforce","controlpoint":99})");
        auto refs = CollectCpReferences(j);
        REQUIRE(refs.size() == 1);
        CHECK(refs[0] == 7);
    }

    TEST_CASE("negative clamps to 0") {
        json j   = json::parse(R"({"name":"controlpointforce","controlpoint":-3})");
        auto refs = CollectCpReferences(j);
        REQUIRE(refs.size() == 1);
        CHECK(refs[0] == 0);
    }
}

TEST_SUITE("WPMapSequenceParse — kCpReferencedFlag bit") {
    TEST_CASE("constant value is 0x10000 per WE parser") {
        CHECK(kCpReferencedFlag == 0x10000u);
    }
}
