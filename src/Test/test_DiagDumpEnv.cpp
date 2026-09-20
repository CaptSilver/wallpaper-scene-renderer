#include <doctest.h>

#include "Utils/DiagDumpEnv.h"

#include <string>

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

    TEST_CASE("a node id that does not fit an int is skipped like a non-number") {
        // Fits a 64-bit long, so strtol reports no error -- the narrowing to
        // int is where it turns into an unrelated (possibly negative) node id
        // that silently selects the wrong node, or none.
        auto ids = utils::parseDiagNodeIdListEnv("1360,99999999999999,1373");
        CHECK(ids == std::vector<int> { 1360, 1373 });
    }

    TEST_CASE("a node id past the range of a long is skipped like a non-number") {
        // Saturates strtol itself (ERANGE + LONG_MAX), which the int-range
        // check alone would only catch by accident of the saturated value.
        auto ids = utils::parseDiagNodeIdListEnv("1360,999999999999999999999999,1373");
        CHECK(ids == std::vector<int> { 1360, 1373 });
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

    TEST_CASE("a change crowded out by the cap is still reported on the next firing") {
        // The cap limits how much one dump prints, not which changes count.  A
        // var that moved in the same firing as maxCount others must stay
        // unseen, or it is dropped for good: its new value would be recorded
        // as the baseline it was never reported against, and the next diff
        // finds it unchanged.
        std::unordered_map<std::string, double>     previous;
        std::vector<std::pair<std::string, double>> current;
        for (int i = 0; i < 9; i++) {
            current.emplace_back("v" + std::to_string(i), 1.0);
        }
        auto first = utils::selectChangedSharedVars(previous, current, 8);
        REQUIRE(first.size() == 8);
        CHECK(first[7].first == "v7");

        // Nothing moves between the two firings: v8 is reported purely because
        // it was never reported in the first place.
        auto second = utils::selectChangedSharedVars(previous, current, 8);
        REQUIRE(second.size() == 1);
        CHECK(second[0].first == "v8");
        CHECK(second[0].second == doctest::Approx(1.0));

        // And once reported it settles -- a third firing has nothing left.
        CHECK(utils::selectChangedSharedVars(previous, current, 8).empty());
    }
}
