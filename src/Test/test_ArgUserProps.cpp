#include <doctest.h>

#include "../../standalone_view/arg.hpp"

TEST_SUITE("standalone_view::arg user-props JSON") {
    TEST_CASE("empty base and empty sets produce an empty object") {
        CHECK(MergeUserPropsJson("", {}) == "{}");
    }

    TEST_CASE("base JSON body and --set pairs merge, base first") {
        CHECK(MergeUserPropsJson(R"({"a":1})", { "b=true" }) == R"({"a":1,"b":true})");
    }

    TEST_CASE("--set pairs are typed bool / number / string and comma-joined in order") {
        CHECK(MergeUserPropsJson("", { "blurfront=false", "lucyrescue=6", "accent=1 0.5 0" }) ==
              R"({"blurfront":false,"lucyrescue":6,"accent":"1 0.5 0"})");
    }

    TEST_CASE("a malformed pair (no '=') is skipped, not fatal") {
        CHECK(MergeUserPropsJson("", { "noequals" }) == "{}");
    }

    TEST_CASE("BuildRuntimeUserPropsJson is empty for no pairs") {
        CHECK(BuildRuntimeUserPropsJson({}).empty());
    }

    TEST_CASE("BuildRuntimeUserPropsJson wraps a delta -- no base merge") {
        CHECK(BuildRuntimeUserPropsJson({ "blurfront=false" }) == R"({"blurfront":false})");
    }
}
