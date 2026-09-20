#include <doctest.h>

#include "Utils/DiagDumpEnv.h"

// Pins the pure env-parsing + change-diff logic behind WEKDE_DIAG_SHARED /
// WEKDE_DIAG_NODES without touching either call site (SceneBackend.cpp's
// PROPEVAL dump, SceneWallpaper.cpp's DRAW node dumps) -- same "test the
// extracted header, not the caller" shape as ConsoleFlushDedup.hpp.
TEST_SUITE("DiagDumpEnv") {
    TEST_CASE("null env yields an empty name list") {
        CHECK(utils::parseDiagNameListEnv(nullptr).empty());
    }

    TEST_CASE("empty env yields an empty name list") {
        CHECK(utils::parseDiagNameListEnv("").empty());
    }

    TEST_CASE("comma-separated names are split and trimmed") {
        auto names = utils::parseDiagNameListEnv("p1x, sunsize ,rotX");
        REQUIRE(names.size() == 3);
        CHECK(names[0] == "p1x");
        CHECK(names[1] == "sunsize");
        CHECK(names[2] == "rotX");
    }

    TEST_CASE("a trailing comma and blank entries do not produce empty names") {
        auto names = utils::parseDiagNameListEnv("p1x,,  ,rotX,");
        REQUIRE(names.size() == 2);
        CHECK(names[0] == "p1x");
        CHECK(names[1] == "rotX");
    }

    TEST_CASE("null env yields an empty node id list") {
        CHECK(utils::parseDiagNodeIdListEnv(nullptr).empty());
    }

    TEST_CASE("node id list parses valid ints and skips malformed entries") {
        auto ids = utils::parseDiagNodeIdListEnv("1360,1365,notanumber,1373");
        CHECK(ids == std::vector<int> { 1360, 1365, 1373 });
    }

    TEST_CASE("node id list trims whitespace around each entry") {
        auto ids = utils::parseDiagNodeIdListEnv(" 1360 , 1365");
        CHECK(ids == std::vector<int> { 1360, 1365 });
    }

    TEST_CASE("first observation reports every current var as changed") {
        std::unordered_map<std::string, double>     previous;
        std::vector<std::pair<std::string, double>> current { { "a", 1.0 }, { "b", 2.0 } };
        auto changed = utils::selectChangedSharedVars(previous, current, 8);
        REQUIRE(changed.size() == 2);
        CHECK(changed[0].first == "a");
        CHECK(changed[0].second == doctest::Approx(1.0));
        CHECK(changed[1].first == "b");
    }

    TEST_CASE("an unchanged value between two firings is not reported") {
        std::unordered_map<std::string, double>     previous;
        std::vector<std::pair<std::string, double>> current { { "a", 1.0 } };
        utils::selectChangedSharedVars(previous, current, 8);                // seeds `previous`
        auto changed = utils::selectChangedSharedVars(previous, current, 8); // same value again
        CHECK(changed.empty());
    }

    TEST_CASE("only the var whose value moved is reported on the next firing") {
        std::unordered_map<std::string, double> previous;
        utils::selectChangedSharedVars(previous, { { "a", 1.0 }, { "b", 2.0 } }, 8);
        auto changed = utils::selectChangedSharedVars(previous, { { "a", 1.0 }, { "b", 3.0 } }, 8);
        REQUIRE(changed.size() == 1);
        CHECK(changed[0].first == "b");
        CHECK(changed[0].second == doctest::Approx(3.0));
    }

    TEST_CASE("more changed vars than the cap only reports the first maxCount") {
        std::unordered_map<std::string, double>     previous;
        std::vector<std::pair<std::string, double>> current { { "a", 1.0 },
                                                              { "b", 2.0 },
                                                              { "c", 3.0 } };
        auto changed = utils::selectChangedSharedVars(previous, current, 2);
        REQUIRE(changed.size() == 2);
        CHECK(changed[0].first == "a");
        CHECK(changed[1].first == "b");
    }
}
