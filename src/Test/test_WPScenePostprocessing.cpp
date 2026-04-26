#include <doctest.h>

#include "wpscene/WPScene.h"

using namespace wallpaper::wpscene;

TEST_SUITE("WPScene.postprocessing") {
    TEST_CASE("absent → empty string") {
        Orthogonalprojection op;
        nlohmann::json       j = R"({"width":1920,"height":1080})"_json;
        REQUIRE(op.FromJson(j));
        CHECK(op.postprocessing == "");
    }

    TEST_CASE("\"low\" → parsed as \"low\"") {
        Orthogonalprojection op;
        nlohmann::json       j =
            R"({"width":1920,"height":1080,"postprocessing":"low"})"_json;
        REQUIRE(op.FromJson(j));
        CHECK(op.postprocessing == "low");
    }

    TEST_CASE("\"medium\" → parsed as \"medium\"") {
        Orthogonalprojection op;
        nlohmann::json       j =
            R"({"width":1920,"height":1080,"postprocessing":"medium"})"_json;
        REQUIRE(op.FromJson(j));
        CHECK(op.postprocessing == "medium");
    }

    TEST_CASE("\"ultra\" gates HDR mip-chain pipeline (per spec Q1)") {
        Orthogonalprojection op;
        nlohmann::json       j =
            R"({"width":1920,"height":1080,"postprocessing":"ultra"})"_json;
        REQUIRE(op.FromJson(j));
        CHECK(op.postprocessing == "ultra");
    }

    TEST_CASE("\"displayhdr\" gates display-HDR pipeline (per spec Q1)") {
        Orthogonalprojection op;
        nlohmann::json       j =
            R"({"width":1920,"height":1080,"postprocessing":"displayhdr"})"_json;
        REQUIRE(op.FromJson(j));
        CHECK(op.postprocessing == "displayhdr");
    }

    TEST_CASE("auto-projection parses postprocessing") {
        Orthogonalprojection op;
        nlohmann::json       j =
            R"({"auto":true,"postprocessing":"ultra"})"_json;
        REQUIRE(op.FromJson(j));
        CHECK(op.auto_ == true);
        CHECK(op.postprocessing == "ultra");
    }
} // WPScene.postprocessing
