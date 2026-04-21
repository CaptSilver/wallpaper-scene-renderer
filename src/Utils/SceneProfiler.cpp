#include "Utils/SceneProfiler.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace wallpaper::profiler
{

namespace
{

struct Registry {
    std::mutex                                  mutex;
    std::unordered_map<const char*, Entry*>     by_ptr;
    // Stable storage — Entry addresses are stable across rehashes because we
    // heap-allocate and the map only holds pointers.  We keep a vector of
    // owned entries so Dump can iterate without re-reading the map.
    std::vector<std::unique_ptr<Entry>>         owned;
};

Registry& registry() {
    static Registry r;
    return r;
}

} // namespace

Entry* GetEntry(const char* name) {
    auto& r = registry();
    // Fast path: attempt lookup under a short lock.  For a truly hot path
    // callers cache the returned pointer in a function-local static, so this
    // is only taken once per call site.
    std::lock_guard<std::mutex> lk(r.mutex);
    auto                        it = r.by_ptr.find(name);
    if (it != r.by_ptr.end()) return it->second;

    auto   owned_entry = std::make_unique<Entry>();
    owned_entry->name  = name;
    Entry* raw         = owned_entry.get();
    r.owned.push_back(std::move(owned_entry));
    r.by_ptr.emplace(name, raw);
    return raw;
}

std::vector<Snapshot> Collect() {
    auto&                       r = registry();
    std::lock_guard<std::mutex> lk(r.mutex);

    std::vector<Snapshot> out;
    out.reserve(r.owned.size());
    for (const auto& ep : r.owned) {
        Snapshot s;
        s.name    = ep->name ? ep->name : "";
        s.count   = ep->count.load(std::memory_order_relaxed);
        s.sum_ns  = ep->sum_ns.load(std::memory_order_relaxed);
        s.min_ns  = ep->min_ns.load(std::memory_order_relaxed);
        s.max_ns  = ep->max_ns.load(std::memory_order_relaxed);
        s.last_ns = ep->last_ns.load(std::memory_order_relaxed);
        // Normalize: an unused entry (count==0) has min_ns == UINT64_MAX;
        // present it as 0 so the table isn't dominated by bogus minima.
        if (s.count == 0) s.min_ns = 0;
        out.push_back(std::move(s));
    }
    return out;
}

void Reset() {
    auto&                       r = registry();
    std::lock_guard<std::mutex> lk(r.mutex);
    for (auto& ep : r.owned) ep->reset();
}

std::string Format(const std::vector<Snapshot>& snaps) {
    std::vector<Snapshot> sorted = snaps;
    std::sort(sorted.begin(), sorted.end(), [](const Snapshot& a, const Snapshot& b) {
        return a.sum_ns > b.sum_ns;
    });

    // Column widths tuned to fit typical names (48ch) and 7-digit ms totals.
    std::ostringstream oss;
    char               header[256];
    std::snprintf(header,
                  sizeof(header),
                  "%-48s %8s %12s %10s %10s %10s %10s\n",
                  "name",
                  "count",
                  "total(ms)",
                  "avg(us)",
                  "min(us)",
                  "max(us)",
                  "last(us)");
    oss << header;

    for (const auto& s : sorted) {
        if (s.count == 0) continue;
        double total_ms = static_cast<double>(s.sum_ns) / 1.0e6;
        double avg_us   = static_cast<double>(s.sum_ns) / static_cast<double>(s.count) / 1.0e3;
        double min_us   = static_cast<double>(s.min_ns) / 1.0e3;
        double max_us   = static_cast<double>(s.max_ns) / 1.0e3;
        double last_us  = static_cast<double>(s.last_ns) / 1.0e3;

        char row[512];
        std::snprintf(row,
                      sizeof(row),
                      "%-48s %8llu %12.3f %10.2f %10.2f %10.2f %10.2f\n",
                      s.name.c_str(),
                      static_cast<unsigned long long>(s.count),
                      total_ms,
                      avg_us,
                      min_us,
                      max_us,
                      last_us);
        oss << row;
    }
    return oss.str();
}

void DumpToStderr() {
    auto snaps = Collect();
    auto text  = Format(snaps);
    std::fputs("[PROFILE] begin ─────────────────────────────────────────────\n", stderr);
    std::fputs(text.c_str(), stderr);
    std::fputs("[PROFILE] end ───────────────────────────────────────────────\n", stderr);
    std::fflush(stderr);
}

} // namespace wallpaper::profiler
