#include "Logging.h"
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>

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

    std::va_list args;
    std::fprintf(stderr, level_fmt[level], level_names[level], file, line);
    {
        va_start(args, fmt);
        std::vfprintf(stderr, fmt, args);
        va_end(args);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    if (auto sink = wallpaper_log_test::getSink()) {
        std::va_list args2;
        va_start(args2, fmt);
        char buf[1024];
        std::vsnprintf(buf, sizeof(buf), fmt, args2);
        va_end(args2);
        sink(level, buf);
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
