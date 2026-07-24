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
//    is inconvenient is not an honesty channel.
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

    // The honesty channel, always rendered.
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

std::string obs_hotedges_dump(const HotEdgeView &v);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_HOTEDGES_H
