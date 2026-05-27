#include <doctest.h>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <random>
#include <atomic>

#include "Vulkan/StagingBufferDetail.hpp"

using wallpaper::vulkan::detail::copyStagingPayload;
using wallpaper::vulkan::detail::growStagingPayload;

namespace
{
// Counts bytes allocated via alloc() and tracks the peak outstanding
// allocation across alloc/free pairs.  Used to pin the MEM3 invariant:
// growing a staging buffer must allocate exactly newsize bytes (one
// buffer) and never briefly hold 2*newsize live.
struct CountingAllocator {
    std::atomic<std::size_t> bytes_allocated { 0 };
    std::atomic<std::size_t> peak_outstanding { 0 };
    std::atomic<std::size_t> outstanding { 0 };

    uint8_t* alloc(std::size_t n) {
        uint8_t* p = static_cast<uint8_t*>(std::malloc(n));
        bytes_allocated.fetch_add(n, std::memory_order_relaxed);
        std::size_t cur       = outstanding.fetch_add(n, std::memory_order_relaxed) + n;
        std::size_t prev_peak = peak_outstanding.load(std::memory_order_relaxed);
        while (cur > prev_peak && ! peak_outstanding.compare_exchange_weak(
                                      prev_peak, cur, std::memory_order_relaxed)) {
        }
        return p;
    }
    void free(uint8_t* p, std::size_t n) {
        std::free(p);
        outstanding.fetch_sub(n, std::memory_order_relaxed);
    }
};
} // namespace

TEST_SUITE("StagingBuffer increaseBuf peak alloc") {
    TEST_CASE("copyStagingPayload identity: same-size copy preserves all bytes") {
        std::mt19937                       rng(42);
        std::uniform_int_distribution<int> dist(0, 255);
        constexpr std::size_t              N = 4096;
        std::vector<uint8_t>               src(N);
        std::vector<uint8_t>               dst(N);
        for (auto& b : src) b = static_cast<uint8_t>(dist(rng));

        copyStagingPayload(src.data(), src.size(), dst.data(), dst.size());

        for (std::size_t i = 0; i < N; ++i) {
            CHECK(dst[i] == src[i]);
        }
    }

    TEST_CASE("copyStagingPayload growth: first old_size bytes preserved, remainder untouched") {
        // increaseBuf shape: src is the old (smaller) buffer, dst the new
        // (larger) buffer.  Only src_size bytes of dst must match src.
        constexpr std::size_t src_size = 1024;
        constexpr std::size_t dst_size = 4096;
        std::vector<uint8_t>  src(src_size, 0xAA);
        std::vector<uint8_t>  dst(dst_size, 0xBB);

        copyStagingPayload(src.data(), src_size, dst.data(), dst_size);

        for (std::size_t i = 0; i < src_size; ++i) {
            CHECK(dst[i] == 0xAA);
        }
        for (std::size_t i = src_size; i < dst_size; ++i) {
            CHECK(dst[i] == 0xBB);
        }
    }

    TEST_CASE("copyStagingPayload clamp: copy size is min(src_size, dst_size)") {
        std::vector<uint8_t> src(2048, 0xCC);
        std::vector<uint8_t> dst(512, 0x00);

        copyStagingPayload(src.data(), src.size(), dst.data(), dst.size());

        for (std::size_t i = 0; i < dst.size(); ++i) {
            CHECK(dst[i] == 0xCC);
        }
    }

    TEST_CASE("copyStagingPayload empty: zero-size copy is a no-op") {
        uint8_t b = 0x55;
        copyStagingPayload(&b, 0, &b, 0);
        CHECK(b == 0x55);
    }

    // ── Peak-allocation invariant (MEM3 load-bearing contract) ───────────────
    //
    // MEM3 pins: growing a staging buffer of newsize bytes must NOT briefly
    // allocate ≈3× peak (old + temp + new).  The two-map design copies old →
    // new directly; the only transient allocation during the grow is the new
    // buffer itself (the old buffer already exists; the new is what we are
    // growing to).
    //
    // We exercise the data-movement seam (growStagingPayload) with a
    // counting allocator and assert the total bytes allocated during the
    // grow equal newsize (one buffer), NOT 2*newsize (temp + new).
    //
    // The grow simulator's "direct" parameter mirrors the production code
    // path: direct=true is the MEM3 path (no temp); direct=false is the
    // pre-MEM3 path (temp vector).  The production StagingBuffer::increaseBuf
    // is wired to the direct=true shape (see Vulkan/StagingBuffer.cpp).

    TEST_CASE("grow allocates only newsize bytes (no 3x temp)") {
        // Seed: old buffer of 1024 bytes filled with 0x42.  Grow to 4096.
        constexpr std::size_t old_size = 1024;
        constexpr std::size_t newsize  = 4096;

        CountingAllocator    alloc;
        std::vector<uint8_t> old_buf(old_size, 0x42);

        // Track per-allocation sizes so we can free correctly.
        std::vector<std::pair<uint8_t*, std::size_t>> live_allocs;

        auto alloc_cb = [&](std::size_t n) -> uint8_t* {
            uint8_t* p = alloc.alloc(n);
            live_allocs.emplace_back(p, n);
            return p;
        };
        auto free_cb = [&](uint8_t* p) {
            auto it = std::find_if(live_allocs.begin(), live_allocs.end(), [p](const auto& kv) {
                return kv.first == p;
            });
            REQUIRE(it != live_allocs.end());
            alloc.free(it->first, it->second);
            live_allocs.erase(it);
        };

        // MEM3 path: direct=true.  Grow old→new directly, no temp.
        uint8_t* new_buf = growStagingPayload(
            old_buf.data(), old_size, newsize, /*direct=*/true, alloc_cb, free_cb);

        // First old_size bytes of new buffer must match old buffer.
        for (std::size_t i = 0; i < old_size; ++i) {
            CHECK(new_buf[i] == 0x42);
        }

        // The load-bearing MEM3 invariant:  the grow must allocate exactly
        // newsize bytes (the new buffer alone), NOT 2*newsize (temp + new).
        // Peak outstanding allocation during the grow must equal newsize:
        // - direct path: one alloc of newsize, no temp.
        // - pre-MEM3 path: alloc temp(newsize) + alloc new(newsize) →
        //   peak outstanding briefly = 2*newsize before tmp is freed.
        CHECK(alloc.bytes_allocated.load() == newsize);
        CHECK(alloc.peak_outstanding.load() == newsize);

        // Clean up.
        free_cb(new_buf);
        CHECK(alloc.outstanding.load() == 0);
    }

    TEST_CASE("grow correctness with random payload") {
        // Defense-in-depth: random bytes round-trip through the grow path.
        std::mt19937                       rng(1337);
        std::uniform_int_distribution<int> dist(0, 255);
        constexpr std::size_t              old_size = 2048;
        constexpr std::size_t              newsize  = 8192;

        std::vector<uint8_t> old_buf(old_size);
        for (auto& b : old_buf) b = static_cast<uint8_t>(dist(rng));

        CountingAllocator                             alloc;
        std::vector<std::pair<uint8_t*, std::size_t>> live;
        auto                                          alloc_cb = [&](std::size_t n) -> uint8_t* {
            uint8_t* p = alloc.alloc(n);
            live.emplace_back(p, n);
            return p;
        };
        auto free_cb = [&](uint8_t* p) {
            auto it = std::find_if(live.begin(), live.end(), [p](const auto& kv) {
                return kv.first == p;
            });
            REQUIRE(it != live.end());
            alloc.free(it->first, it->second);
            live.erase(it);
        };

        uint8_t* new_buf = growStagingPayload(
            old_buf.data(), old_size, newsize, /*direct=*/true, alloc_cb, free_cb);

        for (std::size_t i = 0; i < old_size; ++i) {
            CHECK(new_buf[i] == old_buf[i]);
        }
        CHECK(alloc.bytes_allocated.load() == newsize);
        CHECK(alloc.peak_outstanding.load() == newsize);

        free_cb(new_buf);
    }

} // TEST_SUITE
