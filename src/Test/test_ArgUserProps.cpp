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

TEST_SUITE("standalone_view::arg runtime stepping") {
    TEST_CASE("SEC:key=value splits at the first colon") {
        auto r = ParseRuntimeSetAt("12.5:time=3");
        REQUIRE(r.has_value());
        CHECK(r->delay_s == doctest::Approx(12.5));
        CHECK(r->set == "time=3");
        CHECK_FALSE(ParseRuntimeSetAt("time=3").has_value());
        CHECK_FALSE(ParseRuntimeSetAt("x:time=3").has_value());
        CHECK_FALSE(ParseRuntimeSetAt("5:").has_value());
    }
    TEST_CASE("delays that cannot become a timer interval are rejected") {
        CHECK_FALSE(ParseRuntimeSetAt("nan:k=v").has_value());
        CHECK_FALSE(ParseRuntimeSetAt("inf:k=v").has_value());
        CHECK_FALSE(ParseRuntimeSetAt("1e300:k=v").has_value());
        CHECK_FALSE(ParseRuntimeSetAt("86401:k=v").has_value());
        CHECK_FALSE(ParseRuntimeSetAt("-1:k=v").has_value());
        CHECK(ParseRuntimeSetAt("0:k=v").has_value());
        CHECK(ParseRuntimeSetAt("86400:k=v").has_value());
    }
    TEST_CASE("the burst covers the frames right after a change and one well after") {
        auto f = CaptureBurstFrames(100);
        CHECK(f.front() == 101);
        CHECK(f[7] == 108);
        CHECK(f.back() == 160);
        CHECK(f.size() == 9);
    }
    TEST_CASE(
        "capture paths name the change and the frame, with no path separators from the value") {
        CHECK(ShotAfterSetPath("/out", "time=3", 101) == "/out/time=3_f000101.ppm");
        CHECK(ShotAfterSetPath("/out", "a/b=c", 7) == "/out/a_b=c_f000007.ppm");
    }
}
