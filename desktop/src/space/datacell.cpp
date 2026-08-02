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

} // namespace asmdesk::space
