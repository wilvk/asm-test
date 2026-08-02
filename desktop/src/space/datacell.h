// datacell.h — the three prefix-sum layers over the terrain's DATA half
// (58-memory-data-cell-family.md T2, T3, T4). One TU because all three are the
// same shape: a read of TerrainModel::DataCell's already-ascending `steps` and
// its parallel prefix arrays, with no event rescan and no new channel invented
// (53-3d-catalog-build-roadmap.md §3's rule for this whole family).
//
//   T2  read/write twin relief      — is this address read-mostly or a write
//                                     accumulator? (two mirrored surfaces)
//   T3  working-set tide            — what is being touched NOW vs drifting
//                                     cold? (a windowed delta + a watermark)
//   T4  observed-lifetime pillars   — over what interval is each address
//                                     OBSERVED alive? (front()..back())
//
// Pure and engine-free (D4): only space/terrain.h — no GL, no ImGui, no engine
// header — so all three compile into BOTH desktop binaries and the null test
// harness, exactly like space/canopy.h.
//
// THE FIDELITY RULE THIS WHOLE FILE EXISTS TO HOLD, stated once: absence is
// not zero, three times over.
//   - T2: a direction that was never captured is a MISSING surface. A cell
//     with reads and no recorded writes must not render a flat write surface,
//     because flat reads as "measured zero writes".
//   - T3: a cold cell is a receding WATERMARK at its last crest, not a zero.
//     A cell never touched at all produces no entry at all — so "went cold"
//     and "never touched" cannot look the same.
//   - T4: the pillar is an OBSERVED-TOUCH lifetime, never an allocation
//     lifetime. No producer emits allocation events (recording.cpp's kind list
//     is trace/coverage/.../mem/blame/statediff — no malloc/free/mmap/brk), so
//     the real object may have existed long before the first touch and long
//     after the last. lifetime_pillar_label() states that in those words and a
//     test pins the wording.
#ifndef ASMDESK_SPACE_DATACELL_H
#define ASMDESK_SPACE_DATACELL_H

#include <cstdint>
#include <string>
#include <vector>

#include "space/terrain.h"

namespace asmdesk::space {

// ---------------------------------------------------------------------------
// T2 — read/write twin relief
// ---------------------------------------------------------------------------

// The three shapes the layer exists to separate, named so the legend and the
// renderer share one vocabulary. `None` is a real, reachable case: a cell all
// of whose accesses carried an unrecognised `rw` token has a measured SIZE but
// no measured direction, and gets neither surface.
enum class ReliefShape { None, ReadOnly, WriteOnly, ReadWrite };
// Legend text, one line per shape, in the OBSERVED wording T2 step 4 requires
// (never "this is an RMW cell" where only one direction was recorded).
const char *relief_shape_label(ReliefShape s);

struct ReliefCell {
    uint32_t cell = 0;
    float u = 0.0f, v = 0.0f; // plane-space cell centre, for the draw half

    // PRESENCE, from DataCell::cum_dir (the honest three-state direction), NOT
    // from cum_rw and NOT from "the byte sum is > 0". False means the
    // direction was never observed at-or-before this slice: the renderer must
    // draw NO surface on that side. A zero-height surface would be a lie.
    bool has_read = false;
    bool has_write = false;
    // MAGNITUDE, from the 54 T2 split prefix sums, at the inclusive slice.
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;
    // Bytes counted into cum_size but into NEITHER direction — the "unknown is
    // not zero" invariant made arithmetic. Carried per cell so the HUD can say
    // how much of this cell's traffic has no recorded direction, rather than
    // that traffic silently vanishing between the terrain and this layer.
    uint64_t unknown_bytes = 0;

    // log1p of the byte sums — the surface heights. +Y = read, -Y = write.
    // Meaningless (and never to be drawn) when the matching has_* is false.
    double read_height = 0.0;
    double write_height = 0.0;

    bool torn = false;         // BOTH surfaces are a floor, not a measurement
    uint64_t last_step = 0;    // drill-in target: the last access at-or-before t
    uint64_t touches = 0;      // accesses at-or-before t (for the readout)

    ReliefShape shape() const;
};

struct DataReliefLayer {
    std::vector<ReliefCell> cells; // one per data cell touched at-or-before t
    uint64_t t = 0;                // the slice this was built at

    // Whole-layer facts the HUD states so an empty layer is never mistaken for
    // an empty program (this is the model half of T1's contract).
    bool mem_present = false;
    uint64_t unknown_direction_bytes = 0; // summed over every cell
    uint32_t unknown_direction_cells = 0; // cells with ANY undirected traffic
    uint32_t read_only_cells = 0, write_only_cells = 0, read_write_cells = 0,
             undirected_cells = 0;
    bool torn = false;
};

// Build the twin relief at the inclusive slice [0, t] (t = UINT64_MAX is the
// whole recording). A data cell with no access at-or-before t produces NO
// entry — it has not been touched yet at this slice, which is a different fact
// from "touched, zero bytes". Costs one binary search per data cell, no
// rescan.
DataReliefLayer build_data_relief(const TerrainModel &m, uint64_t t);

// The layer's own legend line: says "observed", so a reader never reads a
// missing surface as a measured zero. Pinned by test.
const char *data_relief_note();

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_DATACELL_H
