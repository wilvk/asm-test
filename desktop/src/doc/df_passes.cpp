// df_passes.cpp — the pure builder of df_passes.h. No ImGui, no I/O.
#include "doc/df_passes.h"

#include <algorithm>
#include <cstdio>

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

} // namespace

std::vector<uint64_t> df_pass_regions(const SegmentedDataflow &seg) {
    std::vector<uint64_t> v;
    for (const DataflowStream &p : seg.passes) {
        // Per pass, from the stream's own definition (streams.h) — so this and
        // the timeline's region column cannot disagree about what a region is.
        std::vector<uint64_t> r = p.regions();
        v.insert(v.end(), r.begin(), r.end());
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

std::string df_pass_desc(const SegmentedDataflow &seg, size_t p) {
    const size_t n = seg.passes.size();
    if (p >= n)
        return {};
    std::string d =
        "pass " + std::to_string(p + 1) + " of " + std::to_string(n);
    if (df_pass_regions(seg).size() < 2)
        return d; // one region, or none stated: nothing to disambiguate
    // This pass's own region(s). One is the producer's shape (one `base` per
    // invocation); more than one can only come from a stitched or hand-authored
    // stream, and is listed rather than silently reduced to the first.
    const std::vector<uint64_t> mine = seg.passes[p].regions();
    if (mine.empty())
        // The other passes state a region and this one does not. Saying
        // "region 0x0" would invent one; the pager says the base is unknown,
        // which is what makes its offsets unanchored.
        return d + " · region unknown";
    d += " · region ";
    for (size_t i = 0; i < mine.size(); i++)
        d += (i ? ", " : "") + hex(mine[i]);
    return d;
}

} // namespace asmdesk
