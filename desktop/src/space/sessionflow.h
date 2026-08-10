// sessionflow.h — the Session flow scene's plane-free model (2026-08-10
// 3d-simplify-and-session-flow spec): the session-strip channels — thread
// lanes, the kernel rail, memory accesses, run seams — as depth-stacked
// RIBBON RATES over stream order. The deliberate shape is smooth aggregate
// surfaces, not per-event marks: one height per bucket per row, smoothed for
// display, so a Firefox-scale session reads as flowing bands rather than a
// forest of spikes.
//
// Pure data (a POD header, no .cpp), the crossing.h / mispred.h pattern:
// built by views/strip_flow.cpp (which needs StripModel, a views/ type, so
// the builder cannot live in this space/-only island) and consumed by
// scene3d/. That split keeps scene3d depending on space/ ONLY (D4).
//
// THE CLAIMS THIS SCENE MUST NOT BLUR:
//   - the along axis is STREAM ORDER, never time (space/crossing.h's ban);
//   - a height is a BUCKETED RATE (events per bucket), smoothed for display
//     only — `counts` keeps the raw per-bucket totals so a reader (and a
//     test) can always recover the unsmoothed fact;
//   - hidden lanes are COUNTED into one aggregate row, never vanished;
//   - mem carries no tid, so the memory row is r/w-shaded, never thread-hued.
#ifndef ASMDESK_SPACE_SESSIONFLOW_H
#define ASMDESK_SPACE_SESSIONFLOW_H

#include <cstdint>
#include <string>
#include <vector>

namespace asmdesk::space {

// The along-axis resolution: every row shares one bucket grid over
// [0, seq_end), so rows compare bucket for bucket.
inline constexpr uint32_t kFlowBuckets = 192;

enum class FlowRowKind : uint8_t {
    Lane,           // one kept thread lane (tid palette hue by lane_ord)
    AggregateLanes, // the hidden lanes, summed — counted, never vanished
    Kernel,         // syscalls per bucket, hued by the bucket's DOMINANT class
    Memory,         // accesses per bucket, r/w split-shaded (mem has no tid)
};

struct FlowRow {
    FlowRowKind kind = FlowRowKind::Lane;
    std::string label;   // "comm [tid]" / "(+N lanes, M events)" / "kernel
                         // crossings" / "memory accesses"
    int64_t tid = -1;    // Lane rows only; -1 elsewhere
    long tgid = -1;      // Lane rows only; the drill-in's pid when known
    uint32_t lane_ord = 0; // palette hue index for Lane rows (model index,
                           // the SAME hue the strip's pc marks use)
    // kFlowBuckets smoothed display heights, normalized to the SCENE's max
    // (1.0 = the busiest bucket anywhere, so rows compare honestly).
    std::vector<float> heights;
    // The RAW per-bucket event counts — smoothing never touches these.
    std::vector<uint64_t> counts;
    // Kernel row only: the bucket's dominant SyscallClass + 1; 0 = no
    // syscalls in the bucket OR a tie (the visible grey bucket rule).
    std::vector<uint8_t> bucket_class;
    uint64_t events = 0; // the row's total (== sum of counts)
};

struct FlowSeam {
    uint32_t bucket = 0;
    std::string label;  // the strip seam's own label, verbatim
    uint8_t kind = 0;   // StripSeamKind's value, opaque here (space/ cannot
                        // name a views/ enum; the label carries the meaning)
};

struct SessionFlowScene {
    // near → far: kept lanes (model order), the aggregate row when anything
    // hid, the kernel row, the memory row. Rows with zero events are dropped
    // EXCEPT kept lanes — a silent thread draws flat, never invented away.
    std::vector<FlowRow> rows;
    std::vector<FlowSeam> seams;
    uint64_t seq_end = 0;
    uint32_t buckets = kFlowBuckets;

    // OFF unless there is a stream to bucket and at least one row.
    // `disabled_reason` is verbatim and NEVER empty when disabled.
    bool enabled = false;
    std::string disabled_reason;

    // The display claim, pinned (the crossing.h dwell_note precedent): the
    // HUD legend states it and a test asserts it verbatim.
    static const char *smoothing_note() {
        return "heights are events per bucket of stream order, smoothed for "
               "display — not time, not duration";
    }
};

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_SESSIONFLOW_H
