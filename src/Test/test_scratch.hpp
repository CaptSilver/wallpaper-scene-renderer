#pragma once

// Where tests put throwaway files.
//
// Explicitly NOT /tmp: it is a tmpfs on the usual dev machines, so anything left
// there is resident memory rather than disk. That matters here because tests are
// not always allowed to clean up after themselves -- mutation testing kills
// mutants by inducing crashes, aborts and timeouts, so destructors and
// remove_all() calls simply never run for a large share of several thousand
// executions. Those leaks accumulated into RAM until systemd-oomd killed the
// whole desktop session.
//
// Preference order:
//   1. $TMPDIR      — honour an explicit override.
//   2. /var/tmp     — disk-backed, and systemd-tmpfiles reaps it at 30 days, so
//                     leaked fixtures age out instead of accruing forever.
//   3. $XDG_CACHE_HOME / $HOME/.cache — disk, but never reaped, so second best.
//   4. /tmp         — last resort; correct behaviour, wrong storage.
//
// Callers add their own per-process suffix (typically the pid) when concurrent
// runs must not collide: Mull runs many copies of one binary at once.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace wallpaper::test
{

inline std::string ScratchBase() {
    const auto usable = [](const char* p) {
        return p && *p && std::filesystem::exists(p);
    };
    if (const char* t = std::getenv("TMPDIR"); usable(t))
        return std::string(t) + "/wek-test-scratch";
    if (std::filesystem::exists("/var/tmp")) return "/var/tmp/wek-test-scratch";
    if (const char* x = std::getenv("XDG_CACHE_HOME"); usable(x))
        return std::string(x) + "/wek-test-scratch";
    if (const char* h = std::getenv("HOME"); usable(h))
        return std::string(h) + "/.cache/wek-test-scratch";
    return "/tmp/wek-test-scratch";
}

/// A scratch directory unique to this process, created on first use.
inline std::string ScratchDir(std::string_view tag, int pid) {
    std::string dir = ScratchBase() + "/" + std::string(tag) + "_" + std::to_string(pid);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace wallpaper::test
