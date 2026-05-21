#pragma once
#include <vector>
#include <algorithm>

namespace wallpaper
{

// Spec 11 — upper-median of rope segment lengths, computed WITHOUT reordering
// `seg_lens` (the GS rope outlier loop reads seg_lens[ai-1] by original index
// after the median is taken) and WITHOUT a per-frame allocation: the caller
// passes a reusable `scratch` buffer that retains capacity across frames, so
// `assign` copies but does not allocate after warm-up.  Returns 0 for empty
// input.  Matches the legacy `auto sorted = seg_lens; nth_element(mid);
// sorted[mid]` result exactly (same multiset, same upper-median index).
inline float ropeSegmentMedian(const std::vector<float>& seg_lens, std::vector<float>& scratch) {
    if (seg_lens.empty()) return 0.0f;
    scratch.assign(seg_lens.begin(), seg_lens.end()); // reused buffer; capacity retained
    auto mid = scratch.begin() + scratch.size() / 2;
    std::nth_element(scratch.begin(), mid, scratch.end());
    return *mid;
}

} // namespace wallpaper
