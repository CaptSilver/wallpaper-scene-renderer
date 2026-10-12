#include <doctest.h>

#include "../../standalone_view/arg.hpp"

#include <limits>

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

TEST_SUITE("standalone_view::arg delay validation") {
    TEST_CASE("an in-range delay becomes milliseconds, boundaries included") {
        CHECK(DelayMs(0.0, 2.0) == 0);
        CHECK(DelayMs(1.5, 2.0) == 1500);
        CHECK(DelayMs(86400.0, 2.0) == 86400000);
    }
    TEST_CASE("a negative, non-finite or absurd delay uses the fallback instead of overflowing") {
        CHECK(DelayMs(-0.1, 2.0) == 2000);
        CHECK(DelayMs(std::numeric_limits<double>::quiet_NaN(), 1.5) == 1500);
        CHECK(DelayMs(std::numeric_limits<double>::infinity(), 1.5) == 1500);
        CHECK(DelayMs(86400.001, 2.0) == 2000);
        CHECK(DelayMs(1e300, 2.0) == 2000);
    }
}

TEST_SUITE("standalone_view::arg media interval") {
    TEST_CASE("zero and positive intervals within a day are accepted") {
        CHECK(MediaIntervalOk(0.0));
        CHECK(MediaIntervalOk(0.5));
        CHECK(MediaIntervalOk(86400.0));
    }
    TEST_CASE("a negative, non-finite or over-long interval is rejected") {
        CHECK_FALSE(MediaIntervalOk(-0.001));
        CHECK_FALSE(MediaIntervalOk(std::numeric_limits<double>::quiet_NaN()));
        CHECK_FALSE(MediaIntervalOk(std::numeric_limits<double>::infinity()));
        CHECK_FALSE(MediaIntervalOk(86400.5));
    }
}

TEST_SUITE("standalone_view::arg media schedule") {
    TEST_CASE("a schedule whose last entry lands within a day is accepted") {
        CHECK(MediaScheduleOk(2.0, 1.0, 3));
        CHECK(MediaScheduleOk(0.0, 0.0, 5));
        CHECK(MediaScheduleOk(86400.0, 5.0, 1));
        CHECK(MediaScheduleOk(86000.0, 200.0, 3)); // last at exactly 86400
        CHECK(MediaScheduleOk(5.0, 5.0, 0));
    }
    TEST_CASE("a bad delay, bad interval or last entry past a day is rejected") {
        CHECK_FALSE(MediaScheduleOk(-1.0, 1.0, 2));
        CHECK_FALSE(MediaScheduleOk(std::numeric_limits<double>::quiet_NaN(), 1.0, 2));
        CHECK_FALSE(MediaScheduleOk(86401.0, 0.0, 1));
        CHECK_FALSE(MediaScheduleOk(1.0, -1.0, 2));
        CHECK_FALSE(MediaScheduleOk(86000.0, 201.0, 3));
    }
}
