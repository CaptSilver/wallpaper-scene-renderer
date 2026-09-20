#include "Logging.h"
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <vector>

#include "Sha.hpp"

// DEBUG uses the same file:line-attributed format as ERROR -- it's the same
// "here's exactly where this came from" need, just gated off by default
// instead of always-on.
constexpr const char* level_names[] = { "INFO", "ERROR", "DEBUG" };
constexpr const char* level_fmt[]   = { "%-5s", "%-5s %s:%d ", "%-5s %s:%d " };

namespace wallpaper {
namespace {
std::atomic<bool>& debugLogFlag() {
    static std::atomic<bool> flag = [] {
        const char* v = std::getenv("WEKDE_LOG_DEBUG");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    return flag;
}
} // namespace

bool LogDebugEnabled() { return debugLogFlag().load(std::memory_order_relaxed); }
void SetLogDebugEnabled(bool enabled) { debugLogFlag().store(enabled, std::memory_order_relaxed); }
} // namespace wallpaper

namespace wallpaper_log_test {
static Sink g_sink = nullptr;
void        setSink(Sink s) { g_sink = s; }
Sink        getSink() { return g_sink; }
} // namespace wallpaper_log_test

void WallpaperLog(int level, const char* file, int line, const char* fmt, ...) {
    // Defensive clamp: level_names/level_fmt are sized to match the
    // LOGLEVEL_* enum (currently 3 entries); a future enumerator added
    // without updating both tables would otherwise dereference an OOB
    // constexpr-array slot and either print garbage or SIGSEGV inside
    // fprintf's %s expansion. Clamp to LOGLEVEL_ERROR specifically (not just
    // "the highest defined level" -- DEBUG is highest by index but is the
    // wrong fallback, since it's silent by default) so the offending call
    // surfaces as an always-visible ERROR line, loud enough that the missing
    // table row is obvious in the stderr stream.
    constexpr int kLevelCount = static_cast<int>(std::size(level_names));
    static_assert(std::size(level_fmt) == std::size(level_names),
                  "level_fmt and level_names must have matching counts");
    if (level < 0 || level >= kLevelCount) level = LOGLEVEL_ERROR;

    // Render prefix + body + newline into one buffer and hand stdio a
    // single write, instead of three separate unlocked calls (fprintf
    // prefix, vfprintf body, fprintf newline). WallpaperLog is reachable
    // from multiple threads now -- WPTexImageParser::Parse() logs a line
    // per texture during a multi-threaded prefetch -- and the old
    // three-call form let concurrent callers interleave mid-line on
    // unbuffered stderr. One write() of the whole line is atomic on a
    // pipe (what journald captures stderr through) up to PIPE_BUF, 4096 on
    // Linux, so anything that fits this stack buffer is also safe from
    // interleaving.
    char stack_buf[4096];

    int prefix_n = std::snprintf(
        stack_buf, sizeof(stack_buf), level_fmt[level], level_names[level], file, line);
    std::size_t prefix_len = prefix_n > 0 ? static_cast<std::size_t>(prefix_n) : 0;

    std::va_list args;
    va_start(args, fmt);
    // A va_list is consumed by the vsnprintf that reads it, so the oversized
    // path below -- which has to render the body a second time, into a big
    // enough buffer -- needs its own copy made before the first pass.
    std::va_list args_retry;
    va_copy(args_retry, args);
    int body_n =
        prefix_len < sizeof(stack_buf)
            ? std::vsnprintf(stack_buf + prefix_len, sizeof(stack_buf) - prefix_len, fmt, args)
            : std::vsnprintf(nullptr, 0, fmt, args); // prefix alone filled the buffer
    va_end(args);
    std::size_t body_len = body_n > 0 ? static_cast<std::size_t>(body_n) : 0;

    // snprintf reports the length it *wanted*, so this is the true line
    // length whether or not the stack buffer could hold it.
    const std::size_t line_len = prefix_len + body_len + 1; // + '\n'

    if (line_len <= sizeof(stack_buf)) {
        va_end(args_retry);
        // Overwrites vsnprintf's terminating NUL; fwrite never reads one.
        stack_buf[line_len - 1] = '\n';
        std::fwrite(stack_buf, 1, line_len, stderr);
    } else {
        // Diagnostics must not lose their tail -- glslang's multi-error parse
        // log is the main shader-translation debugging surface and routinely
        // runs past 4 KB -- so re-render the whole line into an exactly-sized
        // heap buffer and emit that instead. Still one fwrite: a long line
        // can interleave with another thread's (it exceeds PIPE_BUF either
        // way), but a complete line beats an atomic fragment of one.
        // line_len leaves room for the body's terminating NUL, which lands on
        // the last byte and is immediately replaced by the newline.
        std::vector<char> heap_buf(line_len);
        std::snprintf(
            heap_buf.data(), heap_buf.size(), level_fmt[level], level_names[level], file, line);
        std::vsnprintf(heap_buf.data() + prefix_len, heap_buf.size() - prefix_len, fmt, args_retry);
        va_end(args_retry);
        heap_buf[line_len - 1] = '\n';
        std::fwrite(heap_buf.data(), 1, line_len, stderr);
    }
    std::fflush(stderr);
    if (auto sink = wallpaper_log_test::getSink()) {
        std::va_list args2;
        va_start(args2, fmt);
        char sink_buf[1024];
        std::vsnprintf(sink_buf, sizeof(sink_buf), fmt, args2);
        va_end(args2);
        sink(level, sink_buf);
    }
}

std::string logToTmpfileWithSha1(std::span<const char> in, const char* fmt, ...) {
    std::va_list          args;
    std::string           name   = utils::genSha1(in);
    std::filesystem::path fspath = std::filesystem::temp_directory_path() / name;
    std::string           path   = fspath.native();
    auto*                 file   = std::fopen(path.c_str(), "w+");
    {
        va_start(args, fmt);
        std::vfprintf(file, fmt, args);
        va_end(args);
    }
    std::fprintf(file, "\n");
    std::fclose(file);
    return path;
}
