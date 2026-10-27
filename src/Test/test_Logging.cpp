// Coverage for WallpaperLog's defensive level clamp. The two lookup tables
// (level_names, level_fmt) are sized to match the LOGLEVEL_* enum (currently
// 3 entries); an out-of-range level argument would otherwise dereference
// past the end of the constexpr arrays. The clamp falls back to
// LOGLEVEL_ERROR so the offending call surfaces as an ERROR line — the
// sink contract reports the clamped level (the "what was actually logged"
// value), per the documented invariant.
#include <doctest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
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

// wallpaper_log_test's sink hands back a separately-truncated copy of the
// message (see Logging.h) -- it never sees the bytes WallpaperLog actually
// hands to fwrite(stderr). To check the real framing (does every call end
// its own line?) this redirects fd 2 to an unlinked temp file for the
// scope and reads back exactly what stderr received.
class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        char tmpl[] = "/tmp/wek_logtest_XXXXXX";
        m_fd        = mkstemp(tmpl);
        if (m_fd < 0) throw std::runtime_error("StderrCapture: mkstemp failed");
        unlink(tmpl); // fd keeps the file alive; no path needed to read it back
        m_savedStderr = dup(STDERR_FILENO);
        dup2(m_fd, STDERR_FILENO);
    }

    ~StderrCapture() {
        std::fflush(stderr);
        dup2(m_savedStderr, STDERR_FILENO);
        close(m_savedStderr);
        close(m_fd);
    }

    // Everything written to stderr since construction.
    std::string contents() const {
        std::fflush(stderr);
        off_t end = lseek(m_fd, 0, SEEK_CUR);
        lseek(m_fd, 0, SEEK_SET);
        std::string out(static_cast<std::size_t>(end), '\0');
        std::size_t total = 0;
        while (total < out.size()) {
            ssize_t n = read(m_fd, out.data() + total, out.size() - total);
            if (n <= 0) break;
            total += static_cast<std::size_t>(n);
        }
        out.resize(total);
        lseek(m_fd, 0, SEEK_END);
        return out;
    }

private:
    int m_fd;
    int m_savedStderr;
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

// LOG_INFO is gated like LOG_DEBUG: off unless WEKDE_LOG_INFO (or
// WEKDE_LOG_DEBUG) is set.  Pin both sides of the gate so the default can't
// silently flip back to a journald firehose, nor INFO become unreachable.
TEST_SUITE("LOG_INFO gating") {
    TEST_CASE("disabled: LOG_INFO never reaches the sink") {
        SinkGuard  guard;
        const bool was = wallpaper::LogInfoEnabled();
        wallpaper::SetLogInfoEnabled(false);
        LOG_INFO("%s", "should-not-appear");
        CHECK(g_captured_level.load() == -1);
        wallpaper::SetLogInfoEnabled(was);
    }

    TEST_CASE("enabled: LOG_INFO reaches the sink at LOGLEVEL_INFO") {
        SinkGuard  guard;
        const bool was = wallpaper::LogInfoEnabled();
        wallpaper::SetLogInfoEnabled(true);
        LOG_INFO("%s", "marker");
        CHECK(g_captured_level.load() == LOGLEVEL_INFO);
        CHECK(std::string(g_captured_msg) == "marker");
        wallpaper::SetLogInfoEnabled(was);
    }

    TEST_CASE("the environment rule: either variable set, non-empty and not 0") {
        using wallpaper::LogInfoEnabledFromEnv;
        static const char* s_info  = nullptr;
        static const char* s_debug = nullptr;
        auto               rule    = [](const char* a, const char* b) {
            s_info  = a;
            s_debug = b;
            return LogInfoEnabledFromEnv([](const char* name) -> const char* {
                if (std::string(name) == "WEKDE_LOG_INFO") return s_info;
                if (std::string(name) == "WEKDE_LOG_DEBUG") return s_debug;
                return nullptr;
            });
        };
        CHECK_FALSE(rule(nullptr, nullptr)); // the default
        CHECK_FALSE(rule("", ""));
        CHECK_FALSE(rule("0", "0"));
        CHECK(rule("1", nullptr));
        CHECK(rule(nullptr, "1")); // debug implies info
        CHECK(rule("yes", "0"));
        CHECK(rule("0", "1"));
        CHECK_FALSE(rule("", "0"));
    }

    // Each of these switches turns on a diagnostic whose output goes through
    // LOG_INFO, so setting the switch alone has to be enough to see it.
    TEST_CASE("the environment rule: each diagnostic switch turns INFO on") {
        using wallpaper::LogInfoEnabledFromEnv;
        static const char* s_only  = nullptr;
        static const char* s_val   = nullptr;
        const char* const  names[] = { "WEKDE_SCRIPT_DIAG",     "WEKDE_TIME_DIAG",
                                       "WEKDE_DEBUG_FRAMETIME", "WEKDE_DEBUG_SHAKE",
                                       "WEKDE_DEBUG_UNIFORM",   "WEKDE_DEBUG_PARTICLE",
                                       "WEKDE_PIPELINE_DIAG",   "WEKDE_DEBUG_TEXBYTES",
                                       "WEKDE_DIAG_NODES" };
        for (const char* name : names) {
            CAPTURE(name);
            s_only   = name;
            s_val    = "1";
            auto get = [](const char* n) -> const char* {
                return std::string(n) == s_only ? s_val : nullptr;
            };
            CHECK(LogInfoEnabledFromEnv(get));
            s_val = "0";
            CHECK_FALSE(LogInfoEnabledFromEnv(get));
            s_val = "";
            CHECK_FALSE(LogInfoEnabledFromEnv(get));
        }
        CHECK_FALSE(LogInfoEnabledFromEnv([](const char*) -> const char* {
            return nullptr;
        }));
    }

    TEST_CASE("LOG_NOTICE is emitted while INFO is off") {
        SinkGuard  guard;
        const bool was = wallpaper::LogInfoEnabled();
        wallpaper::SetLogInfoEnabled(false);
        LOG_NOTICE("%s", "always-on");
        CHECK(g_captured_level.load() == LOGLEVEL_INFO);
        CHECK(std::string(g_captured_msg) == "always-on");
        wallpaper::SetLogInfoEnabled(was);
    }

    TEST_CASE("ERROR is never gated") {
        SinkGuard  guard;
        const bool was = wallpaper::LogInfoEnabled();
        wallpaper::SetLogInfoEnabled(false);
        LOG_ERROR("%s", "err");
        CHECK(g_captured_level.load() == LOGLEVEL_ERROR);
        wallpaper::SetLogInfoEnabled(was);
    }
}

// WallpaperLog renders prefix + body + newline into one buffer and writes it
// with a single fwrite so concurrent callers (WPTexImageParser::Parse()
// during a multi-threaded texture prefetch) can't interleave mid-line. Two
// properties have to survive a body that outgrows the stack buffer: the line
// is still newline-terminated, and none of it is dropped -- glslang's
// multi-error parse log is the main shader-translation debugging surface and
// runs past the buffer routinely.
TEST_SUITE("WallpaperLog line framing") {
    TEST_CASE("a body wider than the internal buffer is emitted in full") {
        const std::string body(10000, 'x');
        std::string       short_out;
        std::string       long_out;
        {
            StderrCapture capture;
            WallpaperLog(LOGLEVEL_INFO, "", 0, "%s", "m");
            short_out = capture.contents();
        }
        {
            StderrCapture capture;
            WallpaperLog(LOGLEVEL_INFO, "", 0, "%s", body.c_str());
            long_out = capture.contents();
        }

        // The level prefix is whatever level_fmt renders to; measure it off a
        // one-character body instead of restating it (or the buffer size) here.
        REQUIRE(short_out.size() >= 2);
        const std::size_t prefix_len = short_out.size() - 2; // minus "m" and '\n'

        CHECK(long_out.size() == prefix_len + body.size() + 1);
        CHECK(long_out.back() == '\n');
        CHECK(long_out.find(body) == prefix_len);
    }

    TEST_CASE("an oversized line doesn't swallow the line logged after it") {
        std::string   body(10000, 'x');
        StderrCapture capture;
        WallpaperLog(LOGLEVEL_INFO, "", 0, "%s", body.c_str());
        WallpaperLog(LOGLEVEL_INFO, "", 0, "%s", "marker");
        std::string out = capture.contents();

        // Two WallpaperLog calls must produce two lines. If the first call
        // loses its trailing newline, "marker" lands appended to the tail of
        // that line instead of starting a fresh one, and this count drops to 1.
        CHECK(std::count(out.begin(), out.end(), '\n') == 2);
        REQUIRE(out.size() >= 12);
        CHECK(out.substr(out.size() - 12) == "INFO marker\n");
    }
}

// The cases run one at a time, but the code under test does not: AudioCapture
// starts its rebind thread, texture parsing logs from prefetch workers.  Sinks
// append to plain containers, so WallpaperLog has to hand them one line at a
// time, and uninstalling a sink has to wait out a call already in progress.
namespace
{
std::atomic<int> g_insideSink { 0 };
std::atomic<int> g_maxInsideSink { 0 };

void overlapDetectingSink(int, const char*) {
    const int inside = ++g_insideSink;
    int       seen   = g_maxInsideSink.load();
    while (inside > seen && ! g_maxInsideSink.compare_exchange_weak(seen, inside)) {
    }
    // Stay inside long enough for an unserialised caller to arrive meanwhile.
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    --g_insideSink;
}

std::mutex              g_blockMutex;
std::condition_variable g_blockCv;
bool                    g_sinkEntered { false };
bool                    g_sinkReleased { false };

void blockingSink(int, const char*) {
    std::unique_lock lk(g_blockMutex);
    g_sinkEntered = true;
    g_blockCv.notify_all();
    g_blockCv.wait(lk, [] {
        return g_sinkReleased;
    });
}
} // namespace

TEST_SUITE("WallpaperLog sink threading") {
    TEST_CASE("concurrent log calls reach the sink one at a time") {
        g_insideSink    = 0;
        g_maxInsideSink = 0;
        wallpaper_log_test::setSink(&overlapDetectingSink);

        std::vector<std::thread> loggers;
        for (int t = 0; t < 4; ++t) {
            loggers.emplace_back([t] {
                for (int i = 0; i < 25; ++i)
                    WallpaperLog(LOGLEVEL_INFO, "", 0, "t%d line %d", t, i);
            });
        }
        for (auto& th : loggers) th.join();
        wallpaper_log_test::setSink(nullptr);

        CHECK(g_maxInsideSink.load() == 1);
    }

    TEST_CASE("setSink(nullptr) waits for a sink call already in progress") {
        g_sinkEntered  = false;
        g_sinkReleased = false;
        wallpaper_log_test::setSink(&blockingSink);

        std::thread logger([] {
            WallpaperLog(LOGLEVEL_INFO, "", 0, "held");
        });
        {
            std::unique_lock lk(g_blockMutex);
            g_blockCv.wait(lk, [] {
                return g_sinkEntered;
            });
        }

        std::atomic<bool> uninstalled { false };
        std::thread       uninstaller([&] {
            wallpaper_log_test::setSink(nullptr);
            uninstalled = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_FALSE(uninstalled.load());

        {
            std::lock_guard lk(g_blockMutex);
            g_sinkReleased = true;
        }
        g_blockCv.notify_all();
        logger.join();
        uninstaller.join();
        CHECK(uninstalled.load());
    }
}
