// projection.cpp — the Hilbert projection of projection.h. Standard library
// only (D4): no GL, no ImGui, no engine.
#include "space/projection.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "doc/streams.h"

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

// 36 T1 — the rel->abs anchor. Derive the span a routine-relative offset is
// relative to from the one fact the recording states: its codeimage code span.
Anchor resolve_anchor(const std::vector<Region> &regions) {
    Anchor a;

    // Only code spans anchor a routine-relative PC offset; a data/stack/heap/mmap
    // region never makes the anchor ambiguous (a df_step offset is a code offset).
    std::vector<const Region *> code;
    for (const Region &r : regions)
        if (r.kind == Region::Code)
            code.push_back(&r);

    if (code.size() == 1) {
        a.ok = true;
        a.base = code[0]->base;
        a.len = code[0]->len;
        return a;
    }
    if (code.empty()) {
        a.reason = "no codeimage code span — a routine-relative offset has "
                   "nothing to anchor to";
        return a;
    }
    // Two or more: a bare offset carries no region tag on the wire, so which span
    // it belongs to is unrecoverable here. Name each base so the refusal is
    // legible (37 states the region on the wire to resolve this instead of
    // refusing; until then this refuses, louder than a silent empty plane).
    std::string bases;
    for (size_t i = 0; i < code.size(); ++i) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "0x%llx",
                      (unsigned long long)code[i]->base);
        if (i)
            bases += ", ";
        bases += buf;
    }
    a.reason =
        "two or more codeimage code spans (" + bases +
        ") — a routine-relative offset carries no region tag, so the span "
        "it belongs to is unrecoverable";
    return a;
}

bool Anchor::place(uint64_t off, uint64_t *abs) const {
    // An out-of-span offset returns false so the caller COUNTS it rather than
    // dropping it silently — this is the SERVE_CI_MAX_BYTES=4096 clamp case, which
    // is common on real routines, not exotic.
    if (!ok || off >= len)
        return false;
    *abs = base + off;
    return true;
}

namespace {

constexpr uint64_t kPageSize = 4096;

uint64_t page_floor(uint64_t a) { return (a / kPageSize) * kPageSize; }
uint64_t page_ceil(uint64_t a) {
    return ((a + kPageSize - 1) / kPageSize) * kPageSize;
}

struct Span {
    uint64_t lo, hi; // [lo, hi)
};

// Merge spans that touch or overlap once sorted ascending by lo — the
// clustering pass and the page-rounding pass can each independently produce
// adjacency the other did not see, and build_projection's non-overlap
// precondition must hold on the final list.
std::vector<Span> merge_adjacent(std::vector<Span> spans) {
    std::vector<Span> out;
    for (const Span &sp : spans) {
        if (!out.empty() && sp.lo <= out.back().hi)
            out.back().hi = std::max(out.back().hi, sp.hi);
        else
            out.push_back(sp);
    }
    return out;
}

// Clip `sp` against every region in `existing`, returning the (possibly empty,
// possibly two-piece) remainder. `existing` regions do not overlap each other
// (build_projection's own precondition), so each clip is independent.
std::vector<Span> clip_existing(Span sp, const std::vector<Region> &existing) {
    std::vector<Span> pieces{sp};
    for (const Region &r : existing) {
        uint64_t rlo = r.base, rhi = r.base + r.len;
        std::vector<Span> next;
        for (const Span &p : pieces) {
            if (rhi <= p.lo || rlo >= p.hi) {
                next.push_back(p); // no overlap with this region
                continue;
            }
            if (rlo > p.lo)
                next.push_back({p.lo, rlo});
            if (rhi < p.hi)
                next.push_back({rhi, p.hi});
            // rlo <= p.lo && rhi >= p.hi: region swallows p whole — no piece.
        }
        pieces = std::move(next);
    }
    return pieces;
}

} // namespace

std::vector<Region> observed_data_spans(const Recording &rec,
                                        const std::vector<Region> &existing,
                                        std::string *note) {
    // The address sources, in this stated order.
    std::vector<uint64_t> addrs;
    size_t mem_addrs = 0, df_abs_addrs = 0, df_off_skipped = 0;
    if (auto it = rec.by_kind.find("mem"); it != rec.by_kind.end()) {
        for (const Event &e : it->second) {
            addrs.push_back(e.body.value("ea", uint64_t{0}));
            mem_addrs++;
        }
    }
    for (const ValRec &v : decode_streams(rec).df.recs) {
        if (v.space == "abs") {
            addrs.push_back(v.addr);
            df_abs_addrs++;
        } else if (v.space == "off") {
            df_off_skipped++; // region-relative: not placeable raw (36/37 anchor
                              // it instead), and never guessed here
        }
    }

    if (note)
        note->clear();
    if (addrs.empty())
        return {};

    std::sort(addrs.begin(), addrs.end());
    addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());

    // Cluster: open a span at the first address, extend while the next address
    // is within the gap threshold, close and start a new one otherwise.
    std::vector<Span> spans;
    spans.push_back({addrs[0], addrs[0] + 1});
    for (size_t i = 1; i < addrs.size(); ++i) {
        Span &cur = spans.back();
        if (addrs[i] <= cur.hi + kObservedSpanGap)
            cur.hi = addrs[i] + 1;
        else
            spans.push_back({addrs[i], addrs[i] + 1});
    }

    // Round each span out to page boundaries, then re-merge anything the
    // rounding brought into contact.
    for (Span &sp : spans) {
        sp.lo = page_floor(sp.lo);
        sp.hi = page_ceil(sp.hi);
    }
    spans = merge_adjacent(spans);

    // Subtract existing regions so an observed address inside a known region
    // never creates a shadow span.
    std::vector<Span> clipped;
    for (const Span &sp : spans) {
        for (const Span &p : clip_existing(sp, existing))
            if (p.hi > p.lo)
                clipped.push_back(p);
    }

    // Cap the span count: merge the nearest neighbours (smallest gap) until it
    // fits, never drop one.
    bool capped = false;
    while (clipped.size() > kObservedSpanCap) {
        size_t best = 0;
        uint64_t best_gap = UINT64_MAX;
        for (size_t i = 0; i + 1 < clipped.size(); ++i) {
            uint64_t gap = clipped[i + 1].lo - clipped[i].hi;
            if (gap < best_gap) {
                best_gap = gap;
                best = i;
            }
        }
        clipped[best].hi = clipped[best + 1].hi;
        clipped.erase(clipped.begin() + best + 1);
        capped = true;
    }

    std::vector<Region> out;
    out.reserve(clipped.size());
    for (const Span &sp : clipped) {
        Region r;
        r.base = sp.lo;
        r.len = sp.hi - sp.lo;
        r.kind = Region::Unknown;
        r.label = kObservedDataLabel;
        out.push_back(std::move(r));
    }

    if (note) {
        char buf[320];
        std::snprintf(
            buf, sizeof buf,
            "observed data: %zu span%s from %zu observed address%s (%zu mem, "
            "%zu dataflow-abs, %zu dataflow-off skipped), %llu-byte gap "
            "threshold%s",
            out.size(), out.size() == 1 ? "" : "s", addrs.size(),
            addrs.size() == 1 ? "" : "es", mem_addrs, df_abs_addrs,
            df_off_skipped, (unsigned long long)kObservedSpanGap,
            capped ? " (cap merged the nearest spans)" : "");
        *note = buf;
    }
    return out;
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
