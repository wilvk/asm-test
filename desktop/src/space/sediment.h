// sediment.h — residency sediment columns
// (58-memory-data-cell-family.md T6): whether each cell is touched early,
// late, or throughout — the temporal phase the moving terrain slice only
// reveals by scrubbing, stood up with the playhead PAUSED.
//
// Pure and engine-free (D4): only space/terrain.h — no GL, no ImGui, no
// engine — like space/canopy.h and space/datacell.h.
//
// THIS IS THE FRAME-BUDGET RISK OF ITS BRIEF, and it is built against the
// existing degrade path from the start rather than around it. N cells x B
// bands is the densest geometry in the family, so:
//   - the caller passes the SAME cell budget the 3D scrub already uses
//     (ui/progress.h's should_degrade / shell.cpp's kScrubCellBudget), and
//   - when N x B would exceed it, the BAND COUNT is reduced — never the cell
//     set. Dropping cells would silently delete measurements; coarsening bands
//     merges adjacent time windows, which loses temporal resolution and
//     nothing else, and `bands` vs `bands_requested` states that it happened.
//
// Two fidelity rules ride here and are tested (D7):
//   - TF_STAT NEVER MERGES INTO EXACT. Survey residency lives in a SEPARATE
//     vector, never in the exact one — the same isolation invariant the stat
//     Terrain enforces for the height field.
//   - A NEVER-HIT CELL PLACES NO COLUMN. Absence, not a zero nub: a nub reads
//     as a measurement.
#ifndef ASMDESK_SPACE_SEDIMENT_H
#define ASMDESK_SPACE_SEDIMENT_H

#include <cstdint>
#include <string>
#include <vector>

#include "space/terrain.h"

namespace asmdesk::space {

// One band of a column: the hits inside the half-open terrain-time window
// [lo_step, hi_step). Only NON-EMPTY bands are emitted — an empty band is not
// a measured zero, it is simply a window in which this cell was not touched,
// and the column's gap says that better than a zero-height slab would.
struct SedimentBand {
    uint32_t index = 0;  // which band, 0..bands-1 (so gaps stay legible)
    uint64_t lo_step = 0, hi_step = 0; // the window, in terrain-time steps
    uint64_t hits = 0;                 // accesses in [lo_step, hi_step)
};

struct SedimentColumn {
    uint32_t cell = 0;
    float u = 0.0f, v = 0.0f;
    bool is_data = false;    // a DataCell's column, else a CodeCell's
    uint64_t total_hits = 0; // the conservation target: sum(bands.hits)
    std::vector<SedimentBand> bands;
    // The recording is torn: this column's top band is a FLOOR, and everything
    // above it is unobserved rather than empty.
    bool torn_capped = false;
};

struct SedimentColumns {
    // The two populations, structurally separate. `exact` holds columns built
    // from real per-step attribution; `stat` holds SURVEY residency, which has
    // NO per-step attribution at all (see `no_phase` below).
    std::vector<SedimentColumn> exact;
    std::vector<SedimentColumn> stat;
    // A survey column carries magnitude but NOT phase: the survey states where
    // residency landed, never when. Its single band therefore spans the whole
    // axis, and this flag is what stops that from reading as "uniformly hit
    // throughout" — which would be a fabricated temporal claim.
    bool stat_has_no_phase = true;

    uint32_t bands = 0;           // the band count actually used
    uint32_t bands_requested = 0; // what the caller asked for
    bool degraded = false;        // bands < bands_requested, under budget
    uint64_t budget = 0;          // the cell budget the reduction respected
    uint64_t nsteps = 0;          // the terrain-time extent the bands divide
    bool torn = false;
};

// The default band count. Enough to read "early / late / throughout" at a
// glance without turning a column into noise.
inline constexpr uint32_t kSedimentBandsDefault = 16;

// Build the columns over the WHOLE recording (this layer's entire point is to
// be readable with the playhead STATIONARY, so it is not sliced at t).
//
// `bands` is the requested band count (0 => kSedimentBandsDefault).
// `cell_band_budget` is the SAME budget the 3D scrub's should_degrade uses; 0
// means "no budget stated" and the request is honoured as-is. When
// touched_cells * bands would exceed it, `bands` is halved until it fits (a
// floor of 1), `degraded` is set, and NO CELL IS EVER DROPPED.
SedimentColumns build_sediment_columns(const TerrainModel &m,
                                       uint32_t bands = 0,
                                       uint64_t cell_band_budget = 0);

// The layer's legend line — states the axis, the band count (and whether the
// budget coarsened it), the conservation property, and the survey caveat.
std::string sediment_note(const SedimentColumns &cols);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_SEDIMENT_H
