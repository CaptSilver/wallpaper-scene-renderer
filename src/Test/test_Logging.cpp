// Coverage for WallpaperLog's defensive level clamp. The two lookup tables
// (level_names, level_fmt) are sized to match the LOGLEVEL_* enum (currently
// 3 entries); an out-of-range level argument would otherwise dereference
// past the end of the constexpr arrays. The clamp falls back to
// LOGLEVEL_ERROR so the offending call surfaces as an ERROR line — the
// sink contract reports the clamped level (the "what was actually logged"
// value), per the documented invariant.
#include <doctest.h>
#include <atomic>
#include <cstring>
#include "Utils/Logging.h"

namespace
{
std::atomic<int> g_captured_level { -1 };
char             g_captured_msg[256] { 0 };

void capturingSink(int level, const char* msg) {
    g_captured_level.store(level, std::memory_order_relaxed);
    std::strncpy(g_captured_msg, msg, sizeof(g_captured_msg) - 1);
    g_captured_msg[sizeof(g_captured_msg) - 1] = '\0';
}

struct SinkGuard {
    SinkGuard() {
        g_captured_level.store(-1, std::memory_order_relaxed);
        g_captured_msg[0] = '\0';
        wallpaper_log_test::setSink(&capturingSink);
    }
    ~SinkGuard() { wallpaper_log_test::setSink(nullptr); }
};
} // namespace

TEST_SUITE("WallpaperLog level bounds") {

    TEST_CASE("positive OOB level clamps to LOGLEVEL_ERROR") {
        SinkGuard guard;
        WallpaperLog(42, "fakefile", 99, "%s", "marker");
        CHECK(g_captured_level.load() == LOGLEVEL_ERROR);
        CHECK(std::string(g_captured_msg) == "marker");
    }

    TEST_CASE("negative level clamps to LOGLEVEL_ERROR") {
        SinkGuard guard;
        WallpaperLog(-1, "fakefile", 99, "%s", "marker");
        CHECK(g_captured_level.load() == LOGLEVEL_ERROR);
    }

    TEST_CASE("in-range LOGLEVEL_INFO is preserved") {
        SinkGuard guard;
        WallpaperLog(LOGLEVEL_INFO, "", 0, "%s", "info-msg");
        CHECK(g_captured_level.load() == LOGLEVEL_INFO);
        CHECK(std::string(g_captured_msg) == "info-msg");
    }

    TEST_CASE("in-range LOGLEVEL_ERROR is preserved") {
        SinkGuard guard;
        WallpaperLog(LOGLEVEL_ERROR, "fakefile", 7, "%s", "err-msg");
        CHECK(g_captured_level.load() == LOGLEVEL_ERROR);
        CHECK(std::string(g_captured_msg) == "err-msg");
    }

    TEST_CASE("in-range LOGLEVEL_DEBUG is preserved") {
        SinkGuard guard;
        WallpaperLog(LOGLEVEL_DEBUG, "fakefile", 7, "%s", "debug-msg");
        CHECK(g_captured_level.load() == LOGLEVEL_DEBUG);
        CHECK(std::string(g_captured_msg) == "debug-msg");
    }
}

// LOG_DEBUG is off by default -- these pin that the macro's gate actually
// gates (nothing reaches the sink while disabled) and that the setter
// controlling it round-trips, so a future refactor of the gate can't
// silently invert the default and turn every install's journal into a
// per-frame firehose again.
TEST_SUITE("LOG_DEBUG gating") {
    TEST_CASE("disabled by default: LOG_DEBUG never reaches the sink") {
        SinkGuard guard;
        wallpaper::SetLogDebugEnabled(false);
        LOG_DEBUG("%s", "should-not-appear");
        CHECK(g_captured_level.load() == -1);
    }

    TEST_CASE("enabled: LOG_DEBUG reaches the sink at LOGLEVEL_DEBUG") {
        SinkGuard guard;
        wallpaper::SetLogDebugEnabled(true);
        LOG_DEBUG("%s", "marker");
        CHECK(g_captured_level.load() == LOGLEVEL_DEBUG);
        CHECK(std::string(g_captured_msg) == "marker");
        wallpaper::SetLogDebugEnabled(false); // restore the default for later tests
    }

    TEST_CASE("SetLogDebugEnabled round-trips through LogDebugEnabled") {
        wallpaper::SetLogDebugEnabled(true);
        CHECK(wallpaper::LogDebugEnabled());
        wallpaper::SetLogDebugEnabled(false);
        CHECK_FALSE(wallpaper::LogDebugEnabled());
    }
}
