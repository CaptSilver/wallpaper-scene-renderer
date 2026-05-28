// Microbenchmark driver.  Each bench file registers its cases via the
// register_* hooks below; this TU owns the shared ankerl::nanobench::Bench
// instance, the CSV emitter, and the always-exit-0 contract.
//
// CSV format (one row per bench per invocation):
//   timestamp, commit_sha, bench_name, median_ns, stddev_ns
//
// stddev_ns is derived from nanobench's medianAbsolutePercentError (MAPE) by
// scaling the median: mape * median.  The bench-check.sh drift detector
// reads the CSV column-wise so the field order is load-bearing -- do not
// reorder without updating scripts/bench-check.sh.

#define ANKERL_NANOBENCH_IMPLEMENT
#include "nanobench.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>

// Forward decls -- one per bench file.
void register_UniformLookupHash(ankerl::nanobench::Bench&);
void register_RenderGraphTopo(ankerl::nanobench::Bench&);
void register_WPShaderValueUpdater(ankerl::nanobench::Bench&);

namespace
{

std::string getCommitSha() {
    if (const char* s = std::getenv("BENCH_COMMIT_SHA")) return s;
    std::unique_ptr<FILE, int (*)(FILE*)> p(
        popen("git rev-parse --short HEAD 2>/dev/null", "r"), pclose);
    if (!p) return "unknown";
    char buf[64] = { 0 };
    if (!std::fgets(buf, sizeof(buf), p.get())) return "unknown";
    std::string out(buf);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out.empty() ? "unknown" : out;
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    ankerl::nanobench::Bench bench;
    bench.title("backend_scene microbenchmarks")
         .unit("op")
         .warmup(100)
         .relative(true);

    register_UniformLookupHash(bench);
    register_RenderGraphTopo(bench);
    register_WPShaderValueUpdater(bench);

    if (const char* histPath = std::getenv("BENCH_HISTORY")) {
        std::ofstream out(histPath, std::ios::app);
        const std::string sha = getCommitSha();
        const auto now = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        for (const auto& r : bench.results()) {
            const double medianSec
                = r.median(ankerl::nanobench::Result::Measure::elapsed);
            const double mape
                = r.medianAbsolutePercentError(
                    ankerl::nanobench::Result::Measure::elapsed);
            const double stddevNs = mape * medianSec * 1.0e9;
            out << now << ','
                << sha << ','
                << r.config().mBenchmarkName << ','
                << static_cast<long long>(medianSec * 1.0e9) << ','
                << static_cast<long long>(stddevNs) << '\n';
        }
    }
    return 0;
}
