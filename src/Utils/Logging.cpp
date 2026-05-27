#include "Logging.h"
#include <cstdio>
#include <cstdarg>
#include <filesystem>
#include <iterator>

#include "Sha.hpp"

constexpr const char* level_names[] = { "INFO", "ERROR" };
constexpr const char* level_fmt[]   = { "%-5s", "%-5s %s:%d " };

namespace wallpaper_log_test {
static Sink g_sink = nullptr;
void        setSink(Sink s) { g_sink = s; }
Sink        getSink() { return g_sink; }
} // namespace wallpaper_log_test

void WallpaperLog(int level, const char* file, int line, const char* fmt, ...) {
    // Defensive clamp: level_names/level_fmt are sized to match the
    // LOGLEVEL_* enum (currently 2 entries); a future enumerator added
    // without updating both tables would otherwise dereference an OOB
    // constexpr-array slot and either print garbage or SIGSEGV inside
    // fprintf's %s expansion. Clamp into the highest defined level so the
    // offending call surfaces as an ERROR-shaped log line — loud enough
    // that the missing table row is obvious in the stderr stream.
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
