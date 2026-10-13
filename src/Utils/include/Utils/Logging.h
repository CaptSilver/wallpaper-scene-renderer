#pragma once

#include <string>
#include <span>

#define __SHORT_FILE__ __FILE__
#if 1

#    undef __SHORT_FILE__
#    define __SHORT_FILE__ past_last_slash(__FILE__)
/*
        ({                                                       \
            constexpr const char* p = past_last_slash(__FILE__); \
            p;                                                   \
        })
*/
static constexpr const char* past_last_slash(const char* const path, const int pos = 0,
                                             const int last_slash = 0) {
    if (path[pos] == '\0') return &path[last_slash];
    if (path[pos] == '/')
        return past_last_slash(path, pos + 1, pos + 1);
    else
        return past_last_slash(path, pos + 1, last_slash);
}

#endif

// LOGLEVEL_DEBUG is appended after ERROR (not inserted between INFO and
// ERROR) so the two existing numeric values keep meaning everywhere they're
// already stored or compared.
enum
{
    LOGLEVEL_INFO  = 0,
    LOGLEVEL_ERROR = 1,
    LOGLEVEL_DEBUG = 2
};

// Off by default too (see wallpaper::LogInfoEnabled), gated in the macro the
// same way as LOG_DEBUG below.  Scene loads log thousands of INFO lines and
// hover/cursor paths log per event; inside plasmashell every line is a
// journald record.
#define LOG_INFO(...)                                                                     \
    do {                                                                                  \
        if (wallpaper::LogInfoEnabled()) WallpaperLog(LOGLEVEL_INFO, "", 0, __VA_ARGS__); \
    } while (0)
// Ungated INFO: output a bug report depends on (GPU in use, swapchain format
// and present mode, validation warnings, screenshot paths).  Same line format
// LOG_INFO had before it was gated.  Use sparingly -- a call here prints on
// every install, so nothing per-frame or per-asset.
#define LOG_NOTICE(...) WallpaperLog(LOGLEVEL_INFO, "", 0, __VA_ARGS__)
#define LOG_ERROR(...) WallpaperLog(LOGLEVEL_ERROR, __SHORT_FILE__, __LINE__, __VA_ARGS__)

// Off by default (see wallpaper::LogDebugEnabled). The guard sits in the
// macro, not inside WallpaperLog, so a disabled call costs one atomic load
// and never touches vsnprintf/fflush -- callers on a per-frame path (a node
// transform update, a glyph raster) can log at this level without paying
// for it when nobody asked to see it.
#define LOG_DEBUG(...)                                                          \
    do {                                                                        \
        if (wallpaper::LogDebugEnabled())                                       \
            WallpaperLog(LOGLEVEL_DEBUG, __SHORT_FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

void WallpaperLog(int level, const char* file, int line, const char* fmt, ...);

namespace wallpaper {
// Backed by a function-local std::atomic<bool>, seeded once from
// getenv("WEKDE_LOG_DEBUG") (non-empty and not "0" enables it). Default off:
// LOG_DEBUG exists for someone chasing a live bug, not for every install's
// journal.
bool LogDebugEnabled();
void SetLogDebugEnabled(bool enabled);
// Seeded once from getenv("WEKDE_LOG_INFO"), getenv("WEKDE_LOG_DEBUG") or any
// diagnostic switch whose output goes through LOG_INFO (WEKDE_SCRIPT_DIAG,
// WEKDE_TIME_DIAG, ...); any of them non-empty and not "0" enables it:
// whoever asks for a diagnostic wants to see it.  Default off; ERROR and
// LOG_NOTICE always print.  SetLogInfoEnabled is the runtime switch.
bool LogInfoEnabled();
void SetLogInfoEnabled(bool enabled);
// The seeding rule, given a getenv-like lookup (null = unset).
bool LogInfoEnabledFromEnv(const char* (*getenv_fn)(const char* name));
} // namespace wallpaper

// Test-only sink. When non-null, called from WallpaperLog with the rendered
// message (level + formatted body, no trailing newline) after the stderr
// write. Production callers don't see this; runtime cost is one branch
// when unset. Set/reset from doctest only — the backend_scene_tests binary
// runs doctest cases serially in a single thread, so a global function
// pointer is safe; ensure the sink is cleared at the end of each scope.
namespace wallpaper_log_test {
using Sink = void (*)(int level, const char* msg);
void setSink(Sink s);
} // namespace wallpaper_log_test

std::string logToTmpfileWithSha1(std::span<const char>, const char* fmt, ...);
