#include <doctest.h>
#include "Particle/RopeMedian.hpp"
#include <vector>
#include <algorithm>

using wallpaper::ropeSegmentMedian;

// Reference: the exact computation the GS rope gen did before Spec 11.
static float referenceMedian(const std::vector<float>& seg_lens) {
    if (seg_lens.empty()) return 0.0f;
    auto sorted = seg_lens;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    return sorted[sorted.size() / 2];
}

TEST_SUITE("RopeMedian (Spec 11)") {
    TEST_CASE("empty input -> 0 median (matches reference)") {
        std::vector<float> seg, scratch;
        CHECK(ropeSegmentMedian(seg, scratch) == 0.0f);
    }

    TEST_CASE("median equals the reference nth_element-over-copy result") {
        std::vector<float> seg = { 5.0f, 1.0f, 3.0f, 2.0f, 4.0f }; // upper-median index 2 -> 3.0
        std::vector<float> scratch;
        CHECK(ropeSegmentMedian(seg, scratch) == doctest::Approx(referenceMedian(seg)));
        CHECK(ropeSegmentMedian(seg, scratch) == doctest::Approx(3.0f));
    }

    TEST_CASE("with an outlier segment, median is the body value not the outlier") {
        std::vector<float> seg = { 1.0f, 1.1f, 0.9f, 1.05f, 100.0f }; // median index 2 -> ~1.05
        std::vector<float> scratch;
        float              m = ropeSegmentMedian(seg, scratch);
        CHECK(m == doctest::Approx(referenceMedian(seg)));
        CHECK(m < 2.0f); // not the 100.0 outlier
    }

    TEST_CASE("seg_lens is NOT reordered (outlier loop reads it by original index)") {
        std::vector<float>       seg    = { 5.0f, 1.0f, 3.0f, 2.0f, 4.0f };
        const std::vector<float> before = seg;
        std::vector<float>       scratch;
        (void)ropeSegmentMedian(seg, scratch);
        CHECK(seg == before); // original order preserved
    }

    TEST_CASE("scratch buffer is reusable across calls of differing size (no stale read)") {
        std::vector<float> scratch;
        std::vector<float> big = { 9, 8, 7, 6, 5, 4, 3, 2, 1 }; // median index 4 -> 5
        float              m1  = ropeSegmentMedian(big, scratch);
        CHECK(m1 == doctest::Approx(referenceMedian(big)));
        std::vector<float> small = { 2.0f, 1.0f, 3.0f }; // median index 1 -> 2
        float              m2    = ropeSegmentMedian(small, scratch);
        CHECK(m2 == doctest::Approx(referenceMedian(small)));
        CHECK(m2 == doctest::Approx(2.0f));
    }
} // TEST_SUITE RopeMedian (Spec 11)
