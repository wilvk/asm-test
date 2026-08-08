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

// --- 61 T2: the atlas layout ------------------------------------------------
// Four helpers, kept in THIS anonymous namespace beside d2xy/domain_shift for
// the same reason those are here: they are the layout's own arithmetic, and a
// second copy beside a caller would be a second source of truth.

// floor(plane * p / total) with no 128-bit intermediate. plane == 1<<s is a
// power of two, so the product is a SHIFT; where p is large enough that the
// shift would overflow (p >= 2^40 at the order-12 ceiling — a single 1 TiB
// mapping, rare but not impossible), halve the denominator and the shift
// together instead. That costs at most one cell, and cannot accumulate: the
// boundaries are computed independently and the last one is pinned, not
// computed.
uint64_t plane_boundary(uint32_t order, uint64_t p, uint64_t total) {
    if (total == 0)
        return 0;
    uint32_t s = 2 * order; // plane == 1u << s
    while (s > 0 && p != 0 && (p >> (64 - s)) != 0) {
        total = (total >> 1) + (total & 1); // ceil-halve: never reaches 0
        s--;
    }
    return total ? (p << s) / total : 0;
}

// Bytes covered by one cell of region i's rect. max(1, ...) because a region
// can be granted MORE cells than it has bytes (the plane is up to 4x the
// domain), in which case one byte per cell is the finest honest quantisation
// and the rect's tail cells simply decode to nothing.
uint64_t atlas_bytes_per_cell(uint64_t len, uint64_t cells) {
    if (cells == 0)
        return 1;
    const uint64_t bpc = (len + cells - 1) / cells; // ceil
    return bpc ? bpc : 1;
}

// The serpentine walk, both directions. Row-major would put cell w-1 and cell w
// at opposite ends of the rect; reversing odd rows keeps consecutive cells
// adjacent ACROSS THE ROW BREAK. That is the locality Hilbert was bought for,
// kept where it still means something — inside one region.
void atlas_cell(const AtlasRect &r, uint64_t k, uint32_t *x, uint32_t *y) {
    const uint32_t w = r.x1 - r.x0;
    const uint32_t row = uint32_t(k / w);
    uint32_t col = uint32_t(k % w);
    if (row & 1u)
        col = w - 1u - col; // serpentine: odd rows run right-to-left
    *x = r.x0 + col;
    *y = r.y0 + row;
}
uint64_t atlas_ordinal(const AtlasRect &r, uint32_t x, uint32_t y) {
    const uint32_t w = r.x1 - r.x0;
    const uint32_t row = y - r.y0;
    uint32_t col = x - r.x0;
    if (row & 1u)
        col = w - 1u - col; // its own inverse
    return uint64_t(row) * w + col;
}

// The rectangles: an ORDER-PRESERVING binary split. NOT Bruls/Huizing/van Wijk
// squarify — BHvW sorts by descending area, and `regions` is sorted by base
// precisely so memory neighbours become plane neighbours; reordering would
// break that AND break `rects`' index-parallelism with `regions`. This split
// keeps address order, always cuts the LONGER side so aspect ratios stay
// readable, and cuts on an integer cell boundary — so children tile their
// parent exactly at every level, and the root tiling is therefore exact.
//
// Fills *out with this subtree's encoded child reference — a `nodes` index, or
// -(region index + 1) for a leaf — and returns false if the subtree admits no
// realisable split at all, which rebuild_layout answers by falling back to
// Hilbert. Feasibility is CHOSEN, never assumed: the scan that picks `mid`
// considers only splits this rect can actually realise, because a clamp
// applied afterwards can invert (ceil(a/h) + ceil(b/h) can exceed
// ceil((a+b)/h), collapsing the cut past the opposite edge and dividing by
// zero one level down).
bool split_rect(const std::vector<uint64_t> &b, size_t lo, size_t hi,
                AtlasRect r, std::vector<AtlasRect> &rects,
                std::vector<AtlasNode> &nodes, int32_t *out) {
    const uint32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (uint64_t(w) * uint64_t(h) < uint64_t(hi - lo))
        return false; // fewer cells than regions: unrepresentable, not clampable
    if (hi - lo == 1) {
        // The leaf takes the WHOLE rect. This is what makes the tiling exact,
        // and it is why a region's final cell count can differ from its budget
        // by the rounding accumulated down the tree — the budget picks the
        // cut, the geometry picks the area, and the geometry wins.
        rects[lo] = r;
        *out = -static_cast<int32_t>(lo) - 1;
        return true;
    }
    const uint64_t span = b[hi] - b[lo];
    const bool cut_x = w >= h;
    const uint32_t across = cut_x ? h : w; // cells in one column (or row)
    const uint32_t along = cut_x ? w : h;  // columns (or rows) to divide
    if (across == 0)
        return false;
    // Cut the region RUN as near the half-budget as possible — but only among
    // the splits that FIT: each side needs a cell per region, so a candidate is
    // admissible only when the columns it forces on both sides still add up to
    // `along`. Linear, not binary-searched: nreg is small next to the cell
    // count and a scan cannot get the tie-breaking wrong.
    size_t mid = 0;
    uint64_t best = UINT64_MAX;
    uint32_t need_l = 0, need_r = 0;
    for (size_t i = lo + 1; i < hi; i++) {
        const uint32_t nl = uint32_t((i - lo + across - 1) / across);
        const uint32_t nr = uint32_t((hi - i + across - 1) / across);
        if (uint64_t(nl) + uint64_t(nr) > along)
            continue; // this rect cannot realise that split
        const uint64_t left = b[i] - b[lo];
        const uint64_t d = left * 2 > span ? left * 2 - span : span - left * 2;
        if (d < best) {
            best = d;
            mid = i;
            need_l = nl;
            need_r = nr;
        }
    }
    if (best == UINT64_MAX)
        return false; // no realisable split anywhere in the run
    const uint64_t left = b[mid] - b[lo];
    // need_l <= along - need_r is guaranteed by the scan, so this clamp can
    // only narrow — it can no longer invert the rect.
    uint32_t cut = uint32_t((uint64_t(along) * left + span / 2) / span);
    cut = std::min(std::max(cut, need_l), along - need_r);
    AtlasNode nd;
    AtlasRect a = r, c = r;
    if (cut_x) {
        nd.axis = 0;
        nd.cut = r.x0 + cut;
        a.x1 = c.x0 = nd.cut;
    } else {
        nd.axis = 1;
        nd.cut = r.y0 + cut;
        a.y1 = c.y0 = nd.cut;
    }
    // Reserve OUR slot before recursing — the children append to `nodes`.
    const size_t self = nodes.size();
    nodes.push_back(AtlasNode{});
    if (!split_rect(b, lo, mid, a, rects, nodes, &nd.lo))
        return false;
    if (!split_rect(b, mid, hi, c, rects, nodes, &nd.hi))
        return false;
    nodes[self] = nd;
    *out = static_cast<int32_t>(self);
    return true;
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
    // 61 T2: so a default-constructed projection is consistent without a caller
    // remembering to. A no-op under Hilbert, which clears the two vectors.
    rebuild_layout(p);
    return p;
}

// 61 T2: see projection.h for the contract, and the anonymous namespace above
// for split_rect's feasibility rule.
void rebuild_layout(Projection &p) {
    p.rects.clear();
    p.nodes.clear();
    if (p.layout != Projection::Layout::Atlas)
        return;
    const size_t nreg = p.regions.size();
    const uint64_t total = p.domain_off.empty() ? 0 : p.domain_off.back();
    const uint32_t n = uint32_t(1) << p.order;
    const uint64_t plane = uint64_t(n) * n;
    if (nreg == 0 || total == 0 || nreg > plane) {
        p.layout = Projection::Layout::Hilbert; // see the header's note
        return;
    }
    std::vector<uint64_t> b(nreg + 1, 0);
    for (size_t i = 1; i < nreg; i++)
        b[i] = plane_boundary(p.order, p.domain_off[i], total);
    b[nreg] = plane; // PINNED, not computed: the last boundary IS the plane.
    // A cell each, and strictly increasing: a region granted zero cells would
    // get an empty rect that project() could never place an address into.
    // Sweep up, then back down off the pinned top; both fit because nreg <=
    // plane. The upward sweep stops at nreg-1 and NOT at nreg: running it over
    // the pinned entry lets a zero-length trailing region push b[nreg] to
    // plane+1, and the downward sweep starts one below and would never pull it
    // back. Harmless for the tiling — the geometry sets the areas, not the
    // budgets — but it would quietly falsify the pin this comment claims.
    for (size_t i = 1; i < nreg; i++)
        if (b[i] < b[i - 1] + 1)
            b[i] = b[i - 1] + 1;
    for (size_t i = nreg; i-- > 1;)
        if (b[i] + 1 > b[i + 1])
            b[i] = b[i + 1] - 1;
    p.rects.assign(nreg, AtlasRect{});
    p.nodes.reserve(nreg ? nreg - 1 : 0);
    int32_t root = 0;
    if (!split_rect(b, 0, nreg, AtlasRect{0, 0, n, n}, p.rects, p.nodes,
                    &root)) {
        // No realisable tiling. Leave NOTHING half-built: a partially filled
        // `rects` would hand project() a zero-area rect and divide by zero.
        p.rects.clear();
        p.nodes.clear();
        p.layout = Projection::Layout::Hilbert; // see the header's note
    }
}

// 36 T1 — the rel->abs anchor. Derive the span a routine-relative offset is
// relative to from the one fact the recording states: its codeimage code span.
Anchor resolve_anchor(const std::vector<Region> &regions) {
    Anchor a;

    // Only code spans anchor a routine-relative PC offset; a data/stack/heap/mmap
    // region never makes the anchor ambiguous (a df_step offset is a code offset).
    //
    // And only a CODEIMAGE code span. Since the vmmap naming overlay landed, a
    // region can be Code because the kernel mapped it r-xp — libc's text, say,
    // under an observed data touch. That is a fact about the memory, not a
    // statement that this recording captured a routine there, and counting it
    // here would manufacture a second anchor candidate and strand every
    // routine-relative path in the recording behind a refusal that then blames
    // a "codeimage code span" which is really a load. See Region::from_vmmap.
    std::vector<const Region *> code;
    for (const Region &r : regions)
        if (r.kind == Region::Code && !r.from_vmmap)
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

    // 61 T2: the atlas reuses the SAME domain search AND the same refusal above
    // — there is no second address->region resolution to drift. Placed BELOW
    // that refusal deliberately: an address in the gap above `r` still resolves
    // to `r` here, and placing it would be a fabricated location.
    if (layout == Layout::Atlas && lo - 1 < rects.size()) {
        const AtlasRect &rect = rects[lo - 1];
        const uint64_t cells =
            uint64_t(rect.x1 - rect.x0) * uint64_t(rect.y1 - rect.y0);
        // k < cells always: k_max = (len-1)/ceil(len/cells) <=
        // (len-1)*cells/len < cells. That bound is what keeps a region's
        // addresses inside its own rect.
        const uint64_t k = (addr - r.base) / atlas_bytes_per_cell(r.len, cells);
        uint32_t x = 0, y = 0;
        atlas_cell(rect, k, &x, &y);
        *u = (x + 0.5f) / float(uint32_t(1) << order);
        *v = (y + 0.5f) / float(uint32_t(1) << order);
        return true;
    }

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

    if (layout == Layout::Atlas) {
        if (rects.empty())
            return false;
        // O(log regions) descent, NOT a scan over rects — build_terrain calls
        // this once per cell over the whole plane.
        size_t idx = 0;
        if (regions.size() > 1) {
            int32_t cur = 0;
            for (;;) {
                const AtlasNode &nd = nodes[size_t(cur)];
                const int32_t nxt =
                    ((nd.axis == 0 ? x : y) < nd.cut) ? nd.lo : nd.hi;
                if (nxt < 0) {
                    idx = size_t(-(nxt + 1));
                    break;
                }
                cur = nxt;
            }
        }
        const Region &reg = regions[idx];
        const AtlasRect &rect = rects[idx];
        const uint64_t cells =
            uint64_t(rect.x1 - rect.x0) * uint64_t(rect.y1 - rect.y0);
        const uint64_t off =
            atlas_ordinal(rect, x, y) * atlas_bytes_per_cell(reg.len, cells);
        if (off >= reg.len)
            return false; // inside the rect but past the region's bytes — the
                          // SAME refusal the Hilbert path gives a padding cell,
                          // not a clamp onto the last byte, which would be a
                          // fabricated address the user could click and be lied
                          // to about
        if (addr)
            *addr = reg.base + off;
        if (r)
            *r = &reg;
        return true;
    }

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

namespace {
// 61 T9: FNV-1a, byte-wise over each value so the digest is byte-order
// independent and an inserted region cannot collide with a resized one by luck
// of layout.
void fp_mix(uint64_t &h, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (i * 8)) & 0xffull;
        h *= 1099511628211ull;
    }
}
std::string fp_regions(size_t n) {
    return std::to_string(n) + (n == 1 ? " region" : " regions");
}
} // namespace

LayoutFingerprint layout_fingerprint(const Projection &proj) {
    LayoutFingerprint fp;
    if (proj.regions.empty())
        return fp; // no floor to have moved
    fp.valid = true;
    fp.regions = proj.regions.size();
    uint64_t h = 14695981039346656037ull;
    fp_mix(h, proj.order);
    fp_mix(h, static_cast<uint64_t>(proj.layout));
    for (uint64_t d : proj.domain_off)
        fp_mix(h, d); // Hilbert's whole mapping, given order
    for (const AtlasRect &r : proj.rects) {
        fp_mix(h, r.x0);
        fp_mix(h, r.y0);
        fp_mix(h, r.x1);
        fp_mix(h, r.y1);
    }
    fp.digest = h;
    return fp;
}

std::string layout_reflow_note(const LayoutFingerprint &prev,
                               const LayoutFingerprint &now) {
    if (!prev.valid || !now.valid)
        return std::string(); // a first floor is not a floor that moved
    if (prev.digest == now.digest)
        return std::string(); // recomputed, but identical: not a reflow
    if (prev.regions != now.regions)
        return "floor re-laid out: " + fp_regions(prev.regions) + " became " +
               std::to_string(now.regions);
    return "floor re-laid out: " + fp_regions(now.regions) +
           ", extents changed";
}

// 61 T8: see projection.h for the rule, the Hilbert refusal and why the
// legibility threshold is not a fidelity claim. Pure — no GL, no ImGui, no
// camera — so the placement rule is checkable in the null harness beside
// region_cells rather than only through a rendered frame.
std::vector<AtlasLabel> atlas_labels(const Projection &proj,
                                     float min_side_frac) {
    std::vector<AtlasLabel> out;
    // rects is empty under Hilbert AND after a rebuild_layout fallback; the
    // size equality also covers a half-built projection a caller assembled by
    // hand, which this directory's own tests do build.
    if (proj.layout != Projection::Layout::Atlas ||
        proj.rects.size() != proj.regions.size())
        return out;
    const uint32_t n = uint32_t(1) << proj.order;
    out.reserve(proj.rects.size());
    for (size_t i = 0; i < proj.rects.size(); i++) {
        const AtlasRect &r = proj.rects[i];
        const uint32_t rw = r.x1 - r.x0, rh = r.y1 - r.y0;
        const uint64_t cells = uint64_t(rw) * uint64_t(rh);
        // The SHORTER side against a fraction of the plane's side: a sliver has
        // area to spare and nowhere to put a word. See the header for the
        // measurements behind both choices.
        const uint32_t min_side = static_cast<uint32_t>(
            static_cast<float>(n) * (min_side_frac > 0.0f ? min_side_frac : 0.0f));
        if (rw < min_side || rh < min_side)
            continue; // too thin to read — see the header's note
        AtlasLabel l;
        l.u = 0.5f * static_cast<float>(r.x0 + r.x1) / static_cast<float>(n);
        l.v = 0.5f * static_cast<float>(r.y0 + r.y1) / static_cast<float>(n);
        l.cells =
            static_cast<uint32_t>(cells > UINT32_MAX ? UINT32_MAX : cells);
        l.region = i;
        const Region &reg = proj.regions[i];
        l.text = reg.label.empty() ? region_style(reg.kind).name : reg.label;
        out.push_back(std::move(l));
    }
    return out;
}

// 51 T2: see projection.h for the rule and its exactness argument. Kept in
// THIS TU because d2xy/domain_shift live here — re-deriving either one beside
// a caller would be a second copy of load-bearing math, which is exactly what
// project()/unproject() were centralised to avoid.
std::vector<uint32_t> region_cells(const Projection &proj,
                                   size_t region_index) {
    std::vector<uint32_t> out;
    if (region_index >= proj.regions.size() ||
        proj.domain_off.size() < proj.regions.size() + 1)
        return out;
    // 61 T2: the atlas set is the region's RECT — every cell the region OWNS,
    // a superset of the cells its addresses REACH (the rect's rounding tail is
    // owned but does not decode). Ownership is what zoning and labelling want.
    // ABOVE the `hi <= lo` guard below, deliberately: under the atlas a
    // zero-length region still owns a rect (rebuild_layout's strictly
    // increasing clamp grants every region at least one cell), so returning {}
    // there would contradict test_focus's "owns at least one cell" contract.
    // Carries its own `n` because the Hilbert path declares one further down.
    if (proj.layout == Projection::Layout::Atlas) {
        if (region_index >= proj.rects.size())
            return out; // rects is empty under a fallback — see rebuild_layout
        const uint32_t n = uint32_t(1) << proj.order;
        const AtlasRect &r = proj.rects[region_index];
        out.reserve(size_t(r.x1 - r.x0) * size_t(r.y1 - r.y0));
        for (uint32_t y = r.y0; y < r.y1; y++)
            for (uint32_t x = r.x0; x < r.x1; x++)
                out.push_back(y * n + x);
        std::sort(out.begin(), out.end()); // already ascending, but the caller
        return out;                        // is promised sorted and cheap is
    }                                      // cheap
    const uint64_t lo = proj.domain_off[region_index];
    const uint64_t hi = proj.domain_off[region_index + 1];
    if (hi <= lo)
        return out; // a zero-length region owns no cells
    const uint64_t total = proj.domain_off.back();
    const uint32_t n = uint32_t(1) << proj.order;
    const uint32_t shift = domain_shift(proj.order, total);
    // The domain range maps to the CONTIGUOUS Hilbert-index run
    // [lo >> shift, (hi-1) >> shift] — project() computes exactly this index
    // for a single address, so walking the run visits every cell any address
    // of this region can land in, and no other.
    const uint64_t h_lo = lo >> shift;
    const uint64_t h_hi = (hi - 1) >> shift;
    out.reserve(static_cast<size_t>(h_hi - h_lo + 1));
    for (uint64_t h = h_lo; h <= h_hi; h++) {
        uint32_t x = 0, y = 0;
        d2xy(n, h, &x, &y);
        out.push_back(y * n + x);
    }
    // Ascending, no duplicates: consecutive Hilbert indices are distinct
    // cells, but the caller is promised a sorted set, and sorting a run of
    // at most `plane` entries is cheap next to the walk that produced it.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace asmdesk::space
