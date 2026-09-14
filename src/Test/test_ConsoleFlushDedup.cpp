#include <doctest.h>

#include "ConsoleFlushDedup.hpp"

// A script that console.logs the same string every property tick (a common
// way to eyeball a per-frame value while writing a script) would otherwise
// put ~30 identical lines a second into the journal forever. These pin the
// dedup decision in isolation, with no QJSEngine involved.
TEST_SUITE("ConsoleFlushDedup") {
    using wek::qml_helper::ConsoleFlushDedup;

    TEST_CASE("first occurrence of a message logs") {
        ConsoleFlushDedup dedup;
        auto              decision = dedup.recordAndDecide("hello", 0);
        CHECK(decision.shouldLog);
        CHECK(decision.repeatedCount == 0);
    }

    TEST_CASE("a repeat within the window is suppressed") {
        ConsoleFlushDedup dedup;
        dedup.recordAndDecide("hello", 0);
        auto decision = dedup.recordAndDecide("hello", 1);
        CHECK_FALSE(decision.shouldLog);
    }

    TEST_CASE("a repeat after the window logs again with the suppressed count") {
        ConsoleFlushDedup dedup;
        dedup.recordAndDecide("hello", 0); // tick 0: logs
        dedup.recordAndDecide("hello", 1); // suppressed, count -> 1
        dedup.recordAndDecide("hello", 2); // suppressed, count -> 2
        auto decision = dedup.recordAndDecide("hello", ConsoleFlushDedup::kWindowTicks);
        CHECK(decision.shouldLog);
        CHECK(decision.repeatedCount == 2);
    }

    TEST_CASE("distinct messages are tracked independently") {
        ConsoleFlushDedup dedup;
        dedup.recordAndDecide("foo", 0);
        auto decision = dedup.recordAndDecide("bar", 1); // different text, same window
        CHECK(decision.shouldLog);
        CHECK(decision.repeatedCount == 0);
    }

    TEST_CASE("the suppressed count resets after being reported") {
        ConsoleFlushDedup dedup;
        dedup.recordAndDecide("hello", 0);
        dedup.recordAndDecide("hello", 1); // suppressed, count -> 1
        auto reported = dedup.recordAndDecide("hello", ConsoleFlushDedup::kWindowTicks);
        CHECK(reported.shouldLog);
        CHECK(reported.repeatedCount == 1);

        // Immediately re-suppressed at the same tick: the count should have
        // started over at 0, not kept accumulating from the previous window.
        auto decision = dedup.recordAndDecide("hello", ConsoleFlushDedup::kWindowTicks);
        CHECK_FALSE(decision.shouldLog);
        auto reportedAgain = dedup.recordAndDecide("hello", ConsoleFlushDedup::kWindowTicks * 2);
        CHECK(reportedAgain.shouldLog);
        CHECK(reportedAgain.repeatedCount == 1);
    }
}
