// hotedges.h — the statistical hot-edge table (08-observer-views.md T4).
//
// **This is not a profiler flame graph, and the difference is a measurement,
// not a taste.** An IBS-Op sample is a retired branch: a FROM address and a TO
// address. Nothing in it observed a call stack. Stacking those edges into
// frames — which is what a flame graph draws — would render inferred ancestry
// with the same ink as observed fact, and the inference is wrong exactly where
// it matters most (recursion, tail calls, longjmp, JIT thunks). So the view is
// a ranked EDGE table and a frozen graph snapshot, and this file synthesizes no
// parent, no child and no total.
//
// Two more rules ride here:
//
//  - **The chrome is never optional.** `samples`, `branch_samples`, `lost`,
//    `throttled`, the sampler and the window are how a reader knows whether the
//    ranking means anything. A dropped-sample count that is only shown when it
//    is inconvenient is not a fidelity channel.
//
//  - **Statistical stays statistical.** A survey is `exact:false` BY
//    CONSTRUCTION (schema: "Always exact:false"), so a recording that claims
//    otherwise is a producer defect, and the view says so instead of quietly
//    upgrading the data's trust.
//
// The picker's hot ranking reads from the same model: IBS entry-edge evidence
// where the host has it, else the sw-clock survey carrying the weaker-evidence
// label — 07-T5's contract, in the one place that ranks.
#ifndef ASMDESK_VIEWS_HOTEDGES_H
#define ASMDESK_VIEWS_HOTEDGES_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/terrain.h" // 56 T2: apply_coverage_window's Terrain/TerrainModel
#include "views/observer.h"

namespace asmdesk {

// One `survey` edge (mirrors `asmspy_sample_edge_t`).
struct HotEdge {
    uint64_t from_addr = 0, to_addr = 0;
    std::string from; // "func+0xNN [module]", or "0x…" when unresolved
    std::string to;
    uint64_t count = 0;     // samples aggregated on this edge
    uint64_t mispred = 0;   // of those, mispredicted
    uint64_t is_return = 0; // of those, retiring a return
    int rank = 0;           // 1-based, by descending count (deterministic ties)
};

struct HotEdgeView {
    std::vector<HotEdge> edges; // ranked; a FROZEN snapshot, never animated
    std::string sampler;        // "ibs-op" | "sw-clock" | "" when unstated

    // The fidelity channel, always rendered.
    uint64_t samples = 0;
    uint64_t branch_samples = 0;
    uint64_t lost = 0;
    bool throttled = false;
    bool have_window = false;
    uint64_t window_base = 0, window_len = 0;

    size_t snapshots = 0; // `survey` events seen; the LAST one is shown
    ObsChrome chrome;
    ObsSkip skip;

    // Non-empty when the recording's own provenance contradicts what a survey
    // can be (see the header comment). Rendered as a defect in the RECORDING,
    // which is what it is.
    std::string provenance_conflict;
};

// The src × dst edge-count matrix for the ImPlot heatmap chassis
// (15-plotting-and-graph-nav.md T1). Distinct `from` labels are the rows,
// distinct `to` the cols (both in rank / first-seen order — the edges are already
// ranked), and cells[r*cols + c] is that edge's sample count (0 = no such edge).
// It is a direct display of the edge WEIGHTS — never a stack (fidelity R4). Capped
// at `cap` rows and cols so a large snapshot stays a bounded heatmap; `truncated`
// records that some edges were dropped. Pure, so it is unit-tested without ImPlot
// (D4) and the draw only feeds it to PlotHeatmap.
struct HotEdgeMatrix {
    std::vector<std::string> rows;
    std::vector<std::string> cols;
    std::vector<double> cells; // rows.size() * cols.size(), row-major
    bool truncated = false;
};
HotEdgeMatrix obs_hotedges_matrix(const HotEdgeView &v, std::size_t cap);

HotEdgeView obs_hotedges_build(const Recording &r,
                               const ObsLifecycle *lc = nullptr);

// The evidence label. IBS-Op ranks entry edges — a direct observation of the
// event a capture waits for; the sw-clock survey is residency, which is a
// different and weaker claim (07-T5). Never empty.
std::string obs_hotedges_evidence_label(const HotEdgeView &v);

// True when this view's ranking is the weaker (residency) kind.
bool obs_hotedges_weak_evidence(const HotEdgeView &v);

// The always-visible provenance chrome, as one line.
std::string obs_hotedges_chrome_line(const HotEdgeView &v);

// Why there is no flame graph here, for the UI to state where a user would
// look for one.
const char *obs_hotedges_no_flame_note();

// The top `n` edges for the picker's hot ranking (already ranked; this is the
// prefix, and it says so rather than pretending to be the whole story).
std::vector<HotEdge> obs_hotedges_top(const HotEdgeView &v, size_t n);

// 54-3d-catalog-phase0-plumbing.md T7: the DECIDED source for any 3D scene
// layer that draws statistical control-flow edges (L5's kernel-crossing
// spurs, L14's misprediction survey) — never `doc/streams.h`'s `SurveyEdge`
// (see the comment beside it). Sourcing here means the layer and this view's
// own reader can never disagree about the same edge, and the drill-in target
// for a statistical pick is this view itself.
//
// A separate, minimal type from `HotEdge` on purpose — no resolved from/to
// text, no rank, so a layer physically cannot reach for a display field this
// view never promised to keep stable. That is what makes D7 invariant 1 (the
// statistical source stays physically distinct from the exact one) a
// property of the model layer rather than a rendering convention every new
// layer has to remember on its own.
struct HotEdgeForScene {
    uint64_t from_addr = 0, to_addr = 0;
    uint64_t count = 0;
    uint64_t mispred = 0;
    uint64_t is_return = 0;
};
struct HotEdgeSceneView {
    std::vector<HotEdgeForScene> edges; // same order obs_hotedges_build ranks
    // The fidelity a consumer of `edges` MUST render (D7): the
    // "STATISTICAL — survey" label, graded opacity by `sampler` (a crisp
    // "ibs-op" vs. a washed "sw-clock"), never summed into an exact surface.
    // An empty survey yields an empty `edges` and these fields still say why
    // (samples/lost/throttled), rather than a silent zero-count edge.
    std::string sampler;
    uint64_t samples = 0;
    uint64_t branch_samples = 0;
    uint64_t lost = 0;
    bool throttled = false;
    bool have_window = false;
    uint64_t window_base = 0, window_len = 0;
};
HotEdgeSceneView obs_hotedges_for_scene(const HotEdgeView &v);

// 56 T2 (fidelity-and-module-layers): the confidence layer's coverage-window
// mask — mutates `slice.flags` in place, adding space::TF_INWINDOW_EMPTY /
// TF_OUTWINDOW over `model`'s in-domain cells (kind_by_cell != kKindByCellNone)
// against `hv`'s stated window. A no-op — touches NOT ONE flag — when
// `hv.have_window` is false: "whole-process assumed" is the common case (the
// catalog's own §9 note) and must be the graceful one, never a fabricated
// mask. An in-window cell with `slice.height > 0` needs neither bit; it
// already renders as a mound.
void apply_coverage_window(space::Terrain &slice, const space::TerrainModel &model,
                           const HotEdgeSceneView &hv);

std::string obs_hotedges_dump(const HotEdgeView &v);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_HOTEDGES_H
