#include <doctest.h>
#include "Vulkan/DirtySpan.hpp"

using namespace wallpaper::vulkan;

TEST_SUITE("Staging dirty span") {
    TEST_CASE("a fresh span is empty and yields a zero range") {
        DirtySpan s;
        CHECK(s.empty());
        auto r = s.alignedRange(256, 4096);
        CHECK(r.offset == 0u);
        CHECK(r.size == 0u);
    }

    TEST_CASE("a single write becomes the exact hull") {
        DirtySpan s;
        s.extend(100, 40); // [100, 140)
        CHECK_FALSE(s.empty());
        CHECK(s.lo == 100u);
        CHECK(s.hi == 140u);
    }

    TEST_CASE("a zero-size write is ignored") {
        DirtySpan s;
        s.extend(100, 0);
        CHECK(s.empty());
        s.extend(100, 8);
        s.extend(200, 0); // still ignored
        CHECK(s.lo == 100u);
        CHECK(s.hi == 108u);
    }

    TEST_CASE("ascending writes merge to min-offset / max-end") {
        DirtySpan s;
        s.extend(100, 16); // [100,116)
        s.extend(300, 32); // [300,332)
        CHECK(s.lo == 100u);
        CHECK(s.hi == 332u);
    }

    TEST_CASE("descending / out-of-order writes still merge correctly") {
        DirtySpan s;
        s.extend(300, 32); // [300,332)
        s.extend(100, 16); // [100,116)
        CHECK(s.lo == 100u);
        CHECK(s.hi == 332u);
    }

    TEST_CASE("overlapping writes keep the union extent") {
        DirtySpan s;
        s.extend(100, 50); // [100,150)
        s.extend(120, 60); // [120,180)
        CHECK(s.lo == 100u);
        CHECK(s.hi == 180u);
    }

    TEST_CASE("a gap between two writes is covered by one convex hull") {
        // Models a frame where a middle sub-allocation was NOT re-written
        // (skipped/cached pass): the hull still spans both, and that is safe
        // because the gap bytes hold their prior, already-uploaded value.
        DirtySpan s;
        s.extend(0, 64);     // early small UBO
        s.extend(4096, 256); // later block, gap in between
        CHECK(s.lo == 0u);
        CHECK(s.hi == 4352u);
    }

    TEST_CASE("reset clears the hull; a later write starts fresh (frame boundary)") {
        DirtySpan s;
        s.extend(100, 16);
        s.reset();
        CHECK(s.empty());
        s.extend(500, 8); // must NOT merge with the pre-reset interval
        CHECK(s.lo == 500u);
        CHECK(s.hi == 508u);
    }

    TEST_CASE("alignedRange rounds lo down and hi up to the atom") {
        DirtySpan s;
        s.extend(70, 60); // [70,130)
        auto r = s.alignedRange(64, 1u << 20);
        CHECK(r.offset == 64u); // 70 -> 64
        CHECK(r.size == 128u);  // 130 -> 192 ; 192-64 = 128 ; covers [64,192) ⊇ [70,130)
    }

    TEST_CASE("alignedRange with atom 256 covers the hull") {
        DirtySpan s;
        s.extend(300, 100); // [300,400)
        auto r = s.alignedRange(256, 1u << 20);
        CHECK(r.offset == 256u); // 300 -> 256
        CHECK(r.size == 256u);   // 400 -> 512 ; 512-256 = 256 ; covers [256,512) ⊇ [300,400)
    }

    TEST_CASE("alignedRange clamps the rounded end to cap (never past the buffer)") {
        // hi rounds up past the buffer end; must clamp so VMA never gets
        // offset+size > allocationSize (it asserts that).
        DirtySpan          s;
        const VkDeviceSize cap = 4096;
        s.extend(4000, 90); // [4000,4090) ; end rounds up to 4096 with atom 64
        auto r = s.alignedRange(64, cap);
        CHECK(r.offset == 3968u);        // 4000 -> 3968
        CHECK(r.offset + r.size <= cap); // clamped within the buffer
        CHECK(r.offset + r.size == cap); // exactly to the end here
    }

    TEST_CASE("alignedRange treats atom 0 as 1 (no rounding)") {
        DirtySpan s;
        s.extend(70, 60); // [70,130)
        auto r = s.alignedRange(0, 1u << 20);
        CHECK(r.offset == 70u);
        CHECK(r.size == 60u);
    }
} // TEST_SUITE Staging dirty span
