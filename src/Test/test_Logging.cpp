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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
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
