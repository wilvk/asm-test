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

// ---------------------------------------------------------------------------
// T3 — working-set tide
// ---------------------------------------------------------------------------

// How the crest tint was derived. `Split` is the honest per-direction byte
// ratio from 54 T2's split sums, and is what this tree normally has.
//
// `RwFlagOnly` is the DEGRADED form for a recording NONE of whose accesses
// carry a recognisable `rw` token: there is then no ratio to compute, and the
// only thing `cum_rw` can honestly report is whether a direction bit that had
// not been seen before this window FIRST APPEARED inside it. That is strictly
// weaker than "the window contains a write" — cum_rw is a prefix OR, so it is
// monotone and cannot un-set a bit — and this enum exists so the difference is
// LABELLED rather than quietly presented as a ratio (T3 step 4: an OR flag
// shown as a read/write ratio is a fabricated quantity).
enum class TideTint { Split, RwFlagOnly };
const char *tide_tint_label(TideTint tint);

struct TideCell {
    uint32_t cell = 0;
    float u = 0.0f, v = 0.0f;

    // The LIVE mass: bytes accessed within the window (t-W, t]. Two binary
    // searches over the existing prefix arrays, no rescan.
    uint64_t live_bytes = 0;
    double live_height = 0.0; // log1p(live_bytes); 0 when nothing is live
    bool live = false;        // any access landed inside the window

    // The receding WATERMARK: a cold cell's height at its own last crest —
    // the window sum it had when it was last touched, computed over
    // (last_step-W, last_step]. This is a decay, never a zero: a cell that has
    // gone cold has not gone to zero, and drawing it at zero would say it was
    // never touched.
    double watermark_height = 0.0;
    bool cold = false; // touched at-or-before t, but NOT inside the window

    // Direction WITHIN the window, from the split sums (TideTint::Split) —
    // the crest tint. Under TideTint::RwFlagOnly these stay 0/false and
    // `window_has_direction_bit` is the only honest thing the layer can say.
    uint64_t win_read_bytes = 0, win_write_bytes = 0;
    uint64_t win_unknown_bytes = 0; // window traffic with no recorded direction
    // The degraded binary flag, meaningful only under TideTint::RwFlagOnly:
    // a cum_rw bit that had not been seen before this window appeared inside
    // it. See TideTint's own comment for why this is NOT "the window contains
    // a write".
    bool window_first_direction_bit = false;

    bool torn = false;
    uint64_t last_step = 0; // drill-in: the last hitting step at-or-before t
};

struct WorkingSetTide {
    std::vector<TideCell> cells;
    uint64_t t = 0;
    uint64_t window = 0; // W, in TERRAIN-TIME steps (never the exec-step axis)
    TideTint tint = TideTint::Split;
    bool mem_present = false;
    uint32_t live_cells = 0, cold_cells = 0;
    bool torn = false;
};

// The default dwell window, in terrain-time steps. A knob, not a constant of
// nature: exposed on the HUD (HudState::tide_window) so a reader can widen it,
// and NAMED with its unit wherever it is shown — a bare number was the 2026-07-29
// dataviz review's top finding.
inline constexpr uint64_t kTideWindowDefault = 64;

// Build the tide at the inclusive slice [0, t] with dwell window `window`
// (0 => kTideWindowDefault). Cells never touched at-or-before t produce no
// entry, so "cold" and "never touched" are structurally distinguishable.
WorkingSetTide build_working_set_tide(const TerrainModel &m, uint64_t t,
                                      uint64_t window = 0);

// The layer's legend line — states the axis, the unit and the watermark rule.
std::string tide_note(const WorkingSetTide &tide);

// ---------------------------------------------------------------------------
// T4 — observed-lifetime pillars
// ---------------------------------------------------------------------------

enum class PillarDominance { Read, Write, Balanced, Undirected };
const char *pillar_dominance_label(PillarDominance d);

struct LifetimePillar {
    uint32_t cell = 0;
    float u = 0.0f, v = 0.0f;
    // The OBSERVED-TOUCH interval: DataCell::steps.front()..back(), both
    // already precomputed and ascending, so this layer is one pass and no
    // rescan. Y is terrain (trace) time.
    uint64_t first_step = 0, last_step = 0;
    uint64_t touches = 0;
    // first_step == last_step: a single touch. A STUB with a stated
    // zero-length interval, never silently widened to "at least one step".
    bool zero_length = false;
    // The recording is torn AND this pillar's last touch sits at the tail: the
    // interval's top is a FLOOR, not a cap. The renderer draws it open-topped.
    bool open_top = false;
    uint64_t read_bytes = 0, write_bytes = 0, unknown_bytes = 0;
    bool has_read = false, has_write = false;
    PillarDominance dominance = PillarDominance::Undirected;
};

struct LifetimePillars {
    std::vector<LifetimePillar> pillars;
    uint64_t nsteps = 0; // the terrain-time extent the pillars are drawn in
    bool mem_present = false;
    bool torn = false;
    uint32_t open_topped = 0; // how many pillars are floors, not intervals
};

// Build one pillar per touched data cell over the WHOLE recording (the layer
// is a Gantt of the run, not a slice; the playhead reads as a horizontal plane
// THROUGH the pillars — below = born, intersected = live, above = untouched).
LifetimePillars build_lifetime_pillars(const TerrainModel &m);

// The load-bearing label of this whole brief. MUST contain "observed" and MUST
// NOT contain "allocat" — pinned by test, because nothing in the capture layer
// emits allocation events and a pillar that claimed otherwise would be the
// single most plausible-looking lie this family could tell.
const char *lifetime_pillar_label();

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_DATACELL_H
