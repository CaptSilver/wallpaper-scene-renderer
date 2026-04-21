#pragma once

// Lightweight scoped-timer profiler.
//
// When WEK_PROFILING is not defined, all macros expand to nothing so there is
// zero compile-time cost.  When enabled, each WEK_PROFILE_SCOPE expands to a
// RAII timer that accumulates count/sum/min/max/last into a process-wide
// registry keyed on the string literal's address (so lookups are pointer-hash
// cheap, not string-hash).
//
// Thread-safety: the registry uses a mutex for insert and std::atomic for
// per-sample updates.  A scope exit path that hits an already-registered
// name takes no locks.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace wallpaper::profiler
{

struct Entry {
    const char*           name { nullptr };
    std::atomic<uint64_t> count { 0 };
    std::atomic<uint64_t> sum_ns { 0 };
    std::atomic<uint64_t> min_ns { UINT64_MAX };
    std::atomic<uint64_t> max_ns { 0 };
    std::atomic<uint64_t> last_ns { 0 };

    void addSample(uint64_t elapsed_ns) noexcept {
        count.fetch_add(1, std::memory_order_relaxed);
        sum_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
        last_ns.store(elapsed_ns, std::memory_order_relaxed);

        uint64_t cur_min = min_ns.load(std::memory_order_relaxed);
        while (elapsed_ns < cur_min &&
               ! min_ns.compare_exchange_weak(cur_min, elapsed_ns, std::memory_order_relaxed)) {
        }
        uint64_t cur_max = max_ns.load(std::memory_order_relaxed);
        while (elapsed_ns > cur_max &&
               ! max_ns.compare_exchange_weak(cur_max, elapsed_ns, std::memory_order_relaxed)) {
        }
    }

    void reset() noexcept {
        count.store(0, std::memory_order_relaxed);
        sum_ns.store(0, std::memory_order_relaxed);
        min_ns.store(UINT64_MAX, std::memory_order_relaxed);
        max_ns.store(0, std::memory_order_relaxed);
        last_ns.store(0, std::memory_order_relaxed);
    }
};

// Snapshot of an Entry — used by Dump / tests so consumers don't see atomics.
struct Snapshot {
    std::string name;
    uint64_t    count;
    uint64_t    sum_ns;
    uint64_t    min_ns;
    uint64_t    max_ns;
    uint64_t    last_ns;
};

// Registry: lookup-or-create by string-literal pointer.  The returned Entry*
// lives for the process lifetime, so callers can cache it in a function-local
// static (see the WEK_PROFILE_SCOPE macro).
Entry* GetEntry(const char* name);

// Collect a snapshot of every registered entry.  Safe to call concurrently
// with ScopedTimer destruction — the snapshot is a point-in-time read.
std::vector<Snapshot> Collect();

// Reset all entries to zero (keeps registered names).
void Reset();

// Format a snapshot table, sorted by sum_ns descending.  Column widths fit
// typical scene names (up to ~48 chars).  Returns a single string ending
// in '\n' so callers can std::fputs() or write to a log sink.
std::string Format(const std::vector<Snapshot>& snaps);

// Convenience: Collect() + Format() + write to stderr with a [PROFILE] prefix.
void DumpToStderr();

class ScopedTimer
{
public:
    explicit ScopedTimer(Entry* entry) noexcept
        : m_entry(entry), m_start(std::chrono::steady_clock::now()) {}

    ~ScopedTimer() noexcept {
        if (! m_entry) return;
        auto     end = std::chrono::steady_clock::now();
        uint64_t ns  = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - m_start).count());
        m_entry->addSample(ns);
    }

    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    Entry*                                         m_entry;
    std::chrono::steady_clock::time_point          m_start;
};

} // namespace wallpaper::profiler

// ── Macros ────────────────────────────────────────────────────────────────
// Only define WEK_PROFILING via CMake (-DPROFILING=ON) to enable.

#define WEK_PROFILE_CONCAT_INNER(a, b) a##b
#define WEK_PROFILE_CONCAT(a, b)       WEK_PROFILE_CONCAT_INNER(a, b)

#ifdef WEK_PROFILING

// Cache the Entry* in a function-local static so only the first call through
// any given scope touches the registry mutex.  C++11 guarantees thread-safe
// init of function-local statics.
#    define WEK_PROFILE_SCOPE(name_literal)                                             \
        static ::wallpaper::profiler::Entry* WEK_PROFILE_CONCAT(_wek_prof_entry_,       \
                                                                __LINE__) =             \
            ::wallpaper::profiler::GetEntry(name_literal);                              \
        ::wallpaper::profiler::ScopedTimer WEK_PROFILE_CONCAT(_wek_prof_scope_,         \
                                                              __LINE__) {               \
            WEK_PROFILE_CONCAT(_wek_prof_entry_, __LINE__)                              \
        }

#    define WEK_PROFILE_FUNCTION() WEK_PROFILE_SCOPE(__func__)

#else

#    define WEK_PROFILE_SCOPE(name_literal) ((void)0)
#    define WEK_PROFILE_FUNCTION()          ((void)0)

#endif
