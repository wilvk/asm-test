// projection.cpp — the Hilbert projection of projection.h. Standard library
// only (D4): no GL, no ImGui, no engine.
#include "space/projection.h"

#include <algorithm>
#include <utility>

namespace asmdesk::space {

namespace {

// Hilbert curve mapping, iterative. Public-domain algorithm (Wikipedia, "Hilbert
// curve", d2xy/xy2d) — no external dependency. n = 2^order is the cells per side;
// d2xy and xy2d are exact inverses over d in [0, n*n), and consecutive d map to
// 4-neighbour cells (the locality property the whole projection rests on).
void d2xy(uint32_t n, uint64_t d, uint32_t *x, uint32_t *y) {
    uint32_t rx, ry;
    uint64_t t = d;
    *x = *y = 0;
    for (uint32_t s = 1; s < n; s <<= 1) {
        rx = 1u & (uint32_t)(t / 2);
        ry = 1u & (uint32_t)(t ^ rx);
        if (ry == 0) {
            if (rx == 1) {
                *x = s - 1 - *x;
                *y = s - 1 - *y;
            }
            std::swap(*x, *y);
        }
        *x += s * rx;
        *y += s * ry;
        t /= 4;
    }
}

uint64_t xy2d(uint32_t n, uint32_t x, uint32_t y) {
    uint64_t d = 0;
    uint32_t rx, ry;
    for (uint32_t s = n / 2; s > 0; s >>= 1) {
        rx = (x & s) > 0 ? 1u : 0u;
        ry = (y & s) > 0 ? 1u : 0u;
        d += (uint64_t)s * s * ((3 * rx) ^ ry);
        if (ry == 0) {
            if (rx == 1) {
                x = s - 1 - x;
                y = s - 1 - y;
            }
            std::swap(x, y);
        }
    }
    return d;
}

// The bits to drop so the compacted domain fits the clamped plane. In the common
// case the domain fits (order was chosen to hold it) and this is 0 — a pure 1:1
// mapping, which is what makes project∘unproject exact. Only a space whose
// compacted length exceeds 4^12 cells forces order to the [6,12] ceiling; then
// the top of the domain is shifted down onto the plane so d2xy/xy2d never see an
// index past it. The brief clamps order but is silent on this overflow; scaling
// is the minimum needed to keep the Hilbert index in range.
uint32_t domain_shift(uint32_t order, uint64_t total) {
    if (total == 0)
        return 0;
    const uint64_t plane = uint64_t(1) << (2 * order); // 4^order cells
    uint32_t shift = 0;
    while (((total - 1) >> shift) >= plane)
        shift++;
    return shift;
}

} // namespace

Projection build_projection(std::vector<Region> regions) {
    Projection p;

    // Sort by base so memory neighbours become domain — and therefore plane —
    // neighbours. std::sort is not stable, but bases are distinct in a real map;
    // two regions sharing a base are ordered arbitrarily and it does not matter.
    std::sort(regions.begin(), regions.end(),
              [](const Region &a, const Region &b) { return a.base < b.base; });

    // Compact: domain_off is the running prefix sum of lengths, with a trailing
    // total. regions[i] owns the slot [domain_off[i], domain_off[i+1]).
    p.domain_off.reserve(regions.size() + 1);
    uint64_t total = 0;
    for (const Region &r : regions) {
        p.domain_off.push_back(total);
        total += r.len;
    }
    p.domain_off.push_back(total);
    p.regions = std::move(regions);

    // Size the plane: the smallest order in [6, 12] whose 4^order cells hold the
    // compacted domain. total 0 (no regions) yields the floor order.
    uint32_t order = 6;
    while (order < 12 && (uint64_t(1) << (2 * order)) < total)
        order++;
    p.order = order;
    return p;
}

bool Projection::project(uint64_t addr, float *u, float *v) const {
    if (regions.empty())
        return false;

    // Binary search: the last region with base <= addr is the only one that can
    // contain it (regions are sorted and, by precondition, non-overlapping).
    size_t lo = 0, hi = regions.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (regions[mid].base <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return false; // addr is below every region
    const Region &r = regions[lo - 1];
    if (addr - r.base >= r.len)
        return false; // in the gap above r (or r is zero-length): unmapped

    const uint64_t d = domain_off[lo - 1] + (addr - r.base);
    const uint64_t total = domain_off.back();
    const uint32_t n = uint32_t(1) << order;
    const uint64_t h = d >> domain_shift(order, total);
    uint32_t x, y;
    d2xy(n, h, &x, &y);
    *u = (x + 0.5f) / (float)n;
    *v = (y + 0.5f) / (float)n;
    return true;
}

bool Projection::unproject(float u, float v, uint64_t *addr,
                           const Region **r) const {
    if (regions.empty())
        return false;
    if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f)
        return false;

    const uint32_t n = uint32_t(1) << order;
    uint32_t x = (uint32_t)(u * n);
    uint32_t y = (uint32_t)(v * n);
    if (x >= n) // defensive: u a hair under 1.0 can round to n
        x = n - 1;
    if (y >= n)
        y = n - 1;

    const uint64_t total = domain_off.back();
    const uint64_t d = xy2d(n, x, y) << domain_shift(order, total);
    if (d >= total)
        return false; // a padding cell beyond the compacted domain

    // Binary search the domain table: upper_bound gives the first offset strictly
    // greater than d, so the region owning d is one step back. d < total here, so
    // the result is in [1, regions.size()] and idx-1 is a valid region.
    const size_t idx =
        (size_t)(std::upper_bound(domain_off.begin(), domain_off.end(), d) -
                 domain_off.begin());
    const Region &reg = regions[idx - 1];
    if (addr)
        *addr = reg.base + (d - domain_off[idx - 1]);
    if (r)
        *r = &reg;
    return true;
}

RegionStyle region_style(Region::Kind kind) {
    switch (kind) {
    case Region::Code:
        return {0.90f, 0.55f, 0.15f, "code"};
    case Region::Stack:
        return {0.35f, 0.75f, 0.95f, "stack"};
    case Region::Heap:
        return {0.45f, 0.85f, 0.45f, "heap"};
    case Region::Data:
        return {0.80f, 0.80f, 0.40f, "data"};
    case Region::Mmap:
        return {0.70f, 0.50f, 0.85f, "mmap"};
    case Region::Unknown:
        return {0.55f, 0.55f, 0.60f, "unknown"};
    }
    return {0.55f, 0.55f, 0.60f, "unknown"}; // -Wreturn-type: unreachable
}

} // namespace asmdesk::space
