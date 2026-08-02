// sediment.cpp — the implementation of sediment.h. Standard library + the
// terrain model (D4): no GL, no ImGui, no engine.
#include "space/sediment.h"

#include <algorithm>
#include <cmath>

namespace asmdesk::space {

namespace {

void cell_uv(uint32_t cell, uint32_t w, uint32_t h, float *u, float *v) {
    if (w == 0 || h == 0) {
        *u = *v = 0.0f;
        return;
    }
    const uint32_t x = cell % w, y = cell / w;
    *u = (x + 0.5f) / static_cast<float>(w);
    *v = (y + 0.5f) / static_cast<float>(h);
}

// Fill `col.bands` from an ascending step vector by binary search per band —
// no rescan, which is the property that makes N cells x B bands affordable at
// all. Only NON-EMPTY bands are emitted, and the emitted hits ALWAYS sum to
// steps.size() (the conservation property T6 asks a test to assert): the band
// windows tile [0, nsteps] exactly, with the last one closed at the top so a
// step landing on the extent is never lost to a half-open boundary.
void fill_bands(SedimentColumn &col, const std::vector<uint64_t> &steps,
                uint32_t bands, uint64_t nsteps) {
    col.total_hits = steps.size();
    if (steps.empty() || bands == 0)
        return;
    // The axis has to cover every observed step even when nsteps under-reports
    // it (a `mem` stream can outlast the trace's own step count). Extending the
    // top rather than clamping the data is what keeps the conservation
    // property true — a step past the stated extent is still a real step.
    const uint64_t top = std::max<uint64_t>(nsteps, steps.back() + 1);
    for (uint32_t b = 0; b < bands; ++b) {
        // Integer-proportional edges: no band is lost to rounding, and the
        // last band's hi is exactly `top`.
        const uint64_t lo = top * b / bands;
        const uint64_t hi = (b + 1 == bands) ? top : top * (b + 1) / bands;
        if (hi <= lo)
            continue; // a degenerate band on a very short axis: no window
        const auto lo_it =
            std::lower_bound(steps.begin(), steps.end(), lo);
        // The final band is CLOSED at the top so `steps.back() == top - 1`
        // still lands inside it; every other band is half-open.
        const auto hi_it = std::lower_bound(steps.begin(), steps.end(), hi);
        const uint64_t hits = static_cast<uint64_t>(hi_it - lo_it);
        if (hits == 0)
            continue; // a window this cell was not touched in places NO band
        SedimentBand band;
        band.index = b;
        band.lo_step = lo;
        band.hi_step = hi;
        band.hits = hits;
        col.bands.push_back(band);
    }
}

} // namespace

SedimentColumns build_sediment_columns(const TerrainModel &m, uint32_t bands,
                                       uint64_t cell_band_budget) {
    SedimentColumns out;
    out.bands_requested = bands ? bands : kSedimentBandsDefault;
    out.bands = out.bands_requested;
    out.budget = cell_band_budget;
    out.nsteps = m.nsteps;
    out.torn = m.torn;
    if (!m.basis_error.empty())
        return out;

    // Count the cells that will actually place a column — never-hit cells are
    // excluded here as well as below, so the budget is computed against the
    // real geometry rather than the plane size.
    uint64_t touched = 0;
    for (const TerrainModel::CodeCell &cc : m.code)
        if (!cc.steps.empty())
            touched++;
    for (const TerrainModel::DataCell &dc : m.data)
        if (!dc.steps.empty())
            touched++;

    // THE DEGRADE PATH, wired to the caller's existing budget rather than a
    // new throttle of this layer's own. Coarsen the BANDS, never the cell set:
    // dropping cells would silently delete measurements, while merging
    // adjacent time windows loses temporal resolution and nothing else — and
    // `degraded` + `bands_requested` state that it happened.
    if (cell_band_budget > 0 && touched > 0) {
        while (out.bands > 1 &&
               touched * static_cast<uint64_t>(out.bands) > cell_band_budget)
            out.bands /= 2;
        out.degraded = out.bands < out.bands_requested;
    }

    for (const TerrainModel::CodeCell &cc : m.code) {
        if (cc.steps.empty())
            continue; // a never-hit cell places NOTHING. Absence, not a nub.
        SedimentColumn col;
        col.cell = cc.cell;
        col.is_data = false;
        cell_uv(cc.cell, m.w, m.h, &col.u, &col.v);
        col.torn_capped = m.torn;
        fill_bands(col, cc.steps, out.bands, m.nsteps);
        if (!col.bands.empty())
            out.exact.push_back(std::move(col));
    }
    for (const TerrainModel::DataCell &dc : m.data) {
        if (dc.steps.empty())
            continue;
        SedimentColumn col;
        col.cell = dc.cell;
        col.is_data = true;
        cell_uv(dc.cell, m.w, m.h, &col.u, &col.v);
        col.torn_capped = m.torn;
        fill_bands(col, dc.steps, out.bands, m.nsteps);
        if (!col.bands.empty())
            out.exact.push_back(std::move(col));
    }

    // The SEPARATE statistical population. TerrainModel::stat is a height
    // field with no per-cell step list, because a SURVEY HAS NO PER-STEP
    // ATTRIBUTION — it states where sampled residency landed, never when. So a
    // stat column carries magnitude in ONE band spanning the whole axis and
    // `stat_has_no_phase` says why; inventing per-band counts for it would be
    // exactly the fabricated temporal claim this layer must not make. It lives
    // in its own vector, never in `exact` — the isolation invariant made
    // structural rather than a matter of discipline.
    if (m.has_stat) {
        for (size_t i = 0; i < m.stat.height.size(); ++i) {
            if (m.stat.height[i] <= 0.0f)
                continue;
            if (i >= m.stat.flags.size() || (m.stat.flags[i] & TF_STAT) == 0u)
                continue;
            SedimentColumn col;
            col.cell = static_cast<uint32_t>(i);
            col.is_data = false;
            cell_uv(col.cell, m.w, m.h, &col.u, &col.v);
            col.torn_capped = m.torn;
            // The magnitude the survey stated, back out of the log1p the stat
            // terrain stored it as — reported as ONE unattributed band.
            const uint64_t mag = static_cast<uint64_t>(
                std::llround(std::expm1(static_cast<double>(m.stat.height[i]))));
            col.total_hits = mag;
            SedimentBand band;
            band.index = 0;
            band.lo_step = 0;
            band.hi_step = m.nsteps;
            band.hits = mag;
            col.bands.push_back(band);
            out.stat.push_back(std::move(col));
        }
    }
    return out;
}

std::string sediment_note(const SedimentColumns &cols) {
    std::string s = "sediment: each column divides TRACE TIME into " +
                    std::to_string(cols.bands) +
                    " band(s); a band's size is the hit count in that window, "
                    "and the bands of a column SUM to its total hit count. A "
                    "window a cell was not touched in places no band — a gap, "
                    "never a zero slab; a cell never hit at all places no "
                    "column at all.";
    if (cols.degraded)
        s += " COARSENED under the scene budget: " + std::to_string(cols.bands) +
             " bands instead of " + std::to_string(cols.bands_requested) +
             " — temporal resolution was reduced, no cell was dropped.";
    if (!cols.stat.empty())
        s += " STATISTICAL columns are drawn separately and carry NO phase: a "
             "survey states where residency landed, never when, so each is one "
             "unattributed band over the whole axis.";
    if (cols.torn)
        s += " TORN: the top band is a floor — everything above it is "
             "unobserved, not empty.";
    return s;
}

} // namespace asmdesk::space
