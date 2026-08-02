// datacell.cpp — the implementation of datacell.h. Standard library + the
// terrain model (D4): no GL, no ImGui, no engine.
#include "space/datacell.h"

#include <algorithm>
#include <cmath>

namespace asmdesk::space {

namespace {

// The plane-space centre of a cell index, in the SAME (x+0.5)/n convention
// projection.cpp's project() uses — so a layer's u/v and a projected address's
// u/v land on the same point rather than half a cell apart.
void cell_uv(uint32_t cell, uint32_t w, uint32_t h, float *u, float *v) {
    if (w == 0 || h == 0) {
        *u = *v = 0.0f;
        return;
    }
    const uint32_t x = cell % w, y = cell / w;
    *u = (x + 0.5f) / static_cast<float>(w);
    *v = (y + 0.5f) / static_cast<float>(h);
}

// The count of accesses at-or-before `t` — one binary search over the already
// ascending steps, exactly as TerrainModel::slice does. 0 means the cell has
// not been touched yet at this slice.
size_t upto(const TerrainModel::DataCell &dc, uint64_t t) {
    return static_cast<size_t>(
        std::upper_bound(dc.steps.begin(), dc.steps.end(), t) -
        dc.steps.begin());
}

} // namespace

ReliefShape ReliefCell::shape() const {
    if (has_read && has_write)
        return ReliefShape::ReadWrite;
    if (has_read)
        return ReliefShape::ReadOnly;
    if (has_write)
        return ReliefShape::WriteOnly;
    return ReliefShape::None;
}

const char *relief_shape_label(ReliefShape s) {
    switch (s) {
    case ReliefShape::ReadOnly:
        // "no observed writes", never "no writes": the recording states that
        // it saw none, not that none happened.
        return "observed reads only — a peak with no pit (no observed writes "
               "here; the write surface is ABSENT, not zero)";
    case ReliefShape::WriteOnly:
        return "observed writes only — a pit with no peak (no observed reads "
               "here; the read surface is ABSENT, not zero)";
    case ReliefShape::ReadWrite:
        return "observed reads AND writes — a peak and a pit pinched at the "
               "plane (read-modify-write is NOT inferred; both directions were "
               "separately recorded)";
    case ReliefShape::None:
        return "traffic with NO recorded direction — measured size, unmeasured "
               "direction: neither surface is drawn";
    }
    return "";
}

DataReliefLayer build_data_relief(const TerrainModel &m, uint64_t t) {
    DataReliefLayer out;
    out.t = t;
    out.mem_present = m.mem_present;
    out.torn = m.torn;
    if (!m.basis_error.empty())
        return out; // refused: slice() would return a flat plane too
    out.cells.reserve(m.data.size());
    for (const TerrainModel::DataCell &dc : m.data) {
        const size_t n = upto(dc, t);
        if (n == 0)
            continue; // not touched yet at this slice — no entry, not a zero
        const size_t i = n - 1;
        ReliefCell rc;
        rc.cell = dc.cell;
        cell_uv(dc.cell, m.w, m.h, &rc.u, &rc.v);
        rc.touches = n;
        rc.last_step = dc.steps[i];
        rc.read_bytes = i < dc.cum_read_size.size() ? dc.cum_read_size[i] : 0;
        rc.write_bytes = i < dc.cum_write_size.size() ? dc.cum_write_size[i] : 0;
        const uint64_t total = i < dc.cum_size.size() ? dc.cum_size[i] : 0;
        // cum_read + cum_write == cum_size at every index EXCEPT where an
        // access's direction was not recorded (terrain.h's own note). The
        // remainder IS that undirected traffic — computed, never assumed away.
        const uint64_t directed = rc.read_bytes + rc.write_bytes;
        rc.unknown_bytes = total > directed ? total - directed : 0;
        // PRESENCE from cum_dir, never from the byte sums: a zero-byte access
        // moves no bytes but its direction was still observed, and never from
        // cum_rw, which folds an unrecognised token into READ.
        const uint8_t dir = i < dc.cum_dir.size() ? dc.cum_dir[i] : 0u;
        rc.has_read = (dir & DD_READ) != 0u;
        rc.has_write = (dir & DD_WRITE) != 0u;
        rc.read_height = std::log1p(static_cast<double>(rc.read_bytes));
        rc.write_height = std::log1p(static_cast<double>(rc.write_bytes));
        // A torn capture floors BOTH surfaces — the recording is a lower bound
        // on the traffic in either direction, not on one of them.
        rc.torn = m.torn;

        switch (rc.shape()) {
        case ReliefShape::ReadOnly:
            out.read_only_cells++;
            break;
        case ReliefShape::WriteOnly:
            out.write_only_cells++;
            break;
        case ReliefShape::ReadWrite:
            out.read_write_cells++;
            break;
        case ReliefShape::None:
            out.undirected_cells++;
            break;
        }
        if (rc.unknown_bytes > 0) {
            out.unknown_direction_bytes += rc.unknown_bytes;
            out.unknown_direction_cells++;
        }
        out.cells.push_back(rc);
    }
    return out;
}

const char *data_relief_note() {
    return "twin relief: +Y = log(1 + OBSERVED read bytes), -Y = log(1 + "
           "OBSERVED write bytes), at the terrain playhead. A direction that "
           "was never recorded draws NO surface on that side — a hole, not a "
           "flat zero.";
}

// ---------------------------------------------------------------------------
// T3 — working-set tide
// ---------------------------------------------------------------------------

namespace {

// The first index whose step is > `t`; i.e. the count of accesses at-or-before
// t. Shared by the window's two ends.
size_t idx_upto(const TerrainModel::DataCell &dc, uint64_t t) {
    return static_cast<size_t>(
        std::upper_bound(dc.steps.begin(), dc.steps.end(), t) -
        dc.steps.begin());
}

// prefix[i-1] with a 0 floor — the running total BEFORE index i.
uint64_t before(const std::vector<uint64_t> &prefix, size_t i) {
    return (i > 0 && i - 1 < prefix.size()) ? prefix[i - 1] : 0;
}

// The half-open window (t-W, t] as a pair of prefix indices [lo, hi). W >= t
// clamps lo to 0, so an early playhead sees everything so far rather than
// wrapping into a huge unsigned window.
void window_range(const TerrainModel::DataCell &dc, uint64_t t, uint64_t w,
                  size_t *lo, size_t *hi) {
    *hi = idx_upto(dc, t);
    *lo = (t > w) ? idx_upto(dc, t - w) : 0;
    if (*lo > *hi)
        *lo = *hi;
}

} // namespace

const char *tide_tint_label(TideTint tint) {
    switch (tint) {
    case TideTint::Split:
        return "crest tint: OBSERVED read/write byte split within the window";
    case TideTint::RwFlagOnly:
        // Named for exactly what it is. Never "the window contains a write".
        return "crest tint DEGRADED: no access in this recording carried a "
               "recognisable r/w token, so there is no ratio — the flag says "
               "only that a direction BIT first appeared inside the window "
               "(cum_rw is a prefix OR, not a ratio)";
    }
    return "";
}

WorkingSetTide build_working_set_tide(const TerrainModel &m, uint64_t t,
                                      uint64_t window) {
    WorkingSetTide out;
    out.t = t;
    out.window = window ? window : kTideWindowDefault;
    out.mem_present = m.mem_present;
    out.torn = m.torn;
    if (!m.basis_error.empty())
        return out;

    // Which tint the whole layer is entitled to. Split iff SOME access in the
    // recording carried a recognisable direction; otherwise there is no ratio
    // to draw and the degraded flag is the only honest signal (T3 step 4).
    bool any_direction = false;
    for (const TerrainModel::DataCell &dc : m.data)
        if (!dc.cum_dir.empty() && dc.cum_dir.back() != 0u) {
            any_direction = true;
            break;
        }
    out.tint = any_direction ? TideTint::Split : TideTint::RwFlagOnly;

    const uint64_t w = out.window;
    out.cells.reserve(m.data.size());
    for (const TerrainModel::DataCell &dc : m.data) {
        size_t lo = 0, hi = 0;
        window_range(dc, t, w, &lo, &hi);
        if (hi == 0)
            continue; // never touched at-or-before t: NO entry at all, so
                      // "went cold" and "never touched" cannot look the same

        TideCell tc;
        tc.cell = dc.cell;
        cell_uv(dc.cell, m.w, m.h, &tc.u, &tc.v);
        tc.torn = m.torn; // a torn capture floors the window, like the terrain
        tc.last_step = dc.steps[hi - 1];

        // The live mass: two binary searches over the existing prefix arrays,
        // no rescan (window_range did both).
        tc.live_bytes = before(dc.cum_size, hi) - before(dc.cum_size, lo);
        tc.live = hi > lo;
        tc.live_height = std::log1p(static_cast<double>(tc.live_bytes));
        tc.win_read_bytes =
            before(dc.cum_read_size, hi) - before(dc.cum_read_size, lo);
        tc.win_write_bytes =
            before(dc.cum_write_size, hi) - before(dc.cum_write_size, lo);
        const uint64_t directed = tc.win_read_bytes + tc.win_write_bytes;
        tc.win_unknown_bytes =
            tc.live_bytes > directed ? tc.live_bytes - directed : 0;
        // The degraded signal, computed from cum_rw the only way a prefix OR
        // permits: a bit present at the window's end that was absent before it
        // began.
        {
            const uint32_t at_hi = hi > 0 && hi - 1 < dc.cum_rw.size()
                                       ? dc.cum_rw[hi - 1]
                                       : 0u;
            const uint32_t at_lo = lo > 0 && lo - 1 < dc.cum_rw.size()
                                       ? dc.cum_rw[lo - 1]
                                       : 0u;
            tc.window_first_direction_bit = (at_hi & ~at_lo) != 0u;
        }

        if (!tc.live) {
            // COLD: touched at-or-before t, but nothing inside the window. The
            // watermark is this cell's height AT ITS OWN LAST CREST — the same
            // window sum computed as of `last_step` — so a cell that has gone
            // cold recedes rather than snapping to zero. Drawing it at zero
            // would say it was never touched, which is the "absent is not
            // zero" invariant this whole family holds.
            tc.cold = true;
            size_t clo = 0, chi = 0;
            window_range(dc, tc.last_step, w, &clo, &chi);
            const uint64_t crest =
                before(dc.cum_size, chi) - before(dc.cum_size, clo);
            tc.watermark_height = std::log1p(static_cast<double>(crest));
            out.cold_cells++;
        } else {
            out.live_cells++;
        }
        out.cells.push_back(tc);
    }
    return out;
}

std::string tide_note(const WorkingSetTide &tide) {
    std::string s =
        "working set: live crest = log(1 + bytes touched in the last " +
        std::to_string(tide.window) +
        " TRACE-TIME steps) — the terrain playhead's own axis, NOT the "
        "execution step the flat views brush. A cell touched only before the "
        "window recedes to a faded WATERMARK at its own last crest, never to "
        "zero; a cell never touched at all draws nothing at all. ";
    s += tide_tint_label(tide.tint);
    if (tide.torn)
        s += ". TORN: the window is a floor, not a measurement";
    return s;
}

} // namespace asmdesk::space
