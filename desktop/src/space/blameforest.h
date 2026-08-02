// blameforest.h — the blame convergence forest (57-causal-layers.md T4):
// across all attribution cones in a recording, which PRODUCING STEP is the
// shared root cause many sinks trace back to.
//
// Pure and engine-free (D4): the doc/ streams and the space/ models only — no
// GL, no ImGui, no engine.
//
// **A CONVERGENCE IS A SET OVERLAP, NEVER A LINK.** `weight` is the count of
// DISTINCT recorded `blame` cones whose `cone[]` contains that step. Nothing
// here synthesises an edge between two cones, and nothing infers that two
// sinks are related beyond the fact that the recording attributes both to the
// same producing step. `label()` says so in the data, so a renderer cannot
// present the spike as a causal graph.
//
// **`born_untraced` IS A VERDICT, NOT AN ABSENCE.** A cone whose value has no
// traced producer is THE SINK ALONE (doc/streams.h's own words: "the cone is
// the sink alone... never presented as an empty cone"). It contributes its
// sink mark and nothing else, and it can never raise another step's weight —
// a value with no traced producer "converging" with another would be a pure
// artifact of the renderer.
#ifndef ASMDESK_SPACE_BLAMEFOREST_H
#define ASMDESK_SPACE_BLAMEFOREST_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/streams.h"
#include "space/projection.h"

namespace asmdesk::space {

// One producing step that at least one cone traces back to.
//
// Keyed by STEP, not by cell, and deliberately so: two distinct steps that
// happen to project into the same plane cell keep SEPARATE weights. Summing
// them would manufacture a convergence that no single step actually has, which
// is the one arithmetic mistake this layer could make that would look right.
// `cell` is carried for the draw; a renderer that wants a per-cell brightness
// takes the MAX over the entries in a cell, never the sum.
struct BlameConvergence {
    uint32_t step = 0;
    uint32_t weight = 1; // distinct cones whose cone[] contains `step`
    uint32_t cell = 0;
    float u = 0, v = 0;
    bool is_sink = false; // this step is also some cone's own sink
};

// One cone's sink, placed at the `off` the recording states for it.
struct BlameSink {
    uint32_t step = 0;
    uint64_t off = 0;
    uint32_t cell = 0;
    float u = 0, v = 0;
    // The recording's explicit verdict: this value has NO traced producer.
    // Its cone is itself, and it never contributes to another step's weight.
    bool born_untraced = false;
};

struct BlameForest {
    std::vector<BlameConvergence> producers; // ascending by step
    std::vector<BlameSink> sinks;            // in `Streams::blame` order

    uint32_t cones = 0;          // total `blame` attributions considered
    uint32_t born_untraced = 0;  // of those, how many carry the verdict
    uint32_t max_weight = 0;     // 0 when there are no producers at all

    // A place the T1 placer refused: a sink `off` or a cone step that could
    // not be resolved. Counted, never dropped, and it emits no beacon.
    uint64_t off_plane = 0;
    std::string off_plane_note; // StepPlacer::note(), verbatim

    // Under truncation every cone is a LOWER BOUND, and a convergence that
    // does not appear is NOT SEEN rather than absent. Carried so the caller
    // reuses the slice view's existing banner rather than new wording.
    bool truncated = false;

    bool enabled = false;
    std::string disabled_reason;

    // A recording that blames ONE sink has nothing to converge. The layer must
    // then LOOK like a faint single bundle rather than like a finding — this
    // flag is what a renderer keys that on, so "degrade honestly" is a
    // property of the model rather than a rendering habit.
    bool single_cone = false;

    static const char *label() {
        return "convergence = a SET OVERLAP of recorded blame cones — not a "
               "link between them, and not a synthesised edge";
    }
    static const char *born_untraced_label() {
        return "born untraced — this value has no traced producer; its cone is "
               "the sink alone and never converges with another";
    }
};

// Build the forest from a recording's `blame` attributions.
//
// `df` is the pass those attributions belong to (the placer resolves each
// step's address through it); `truncated` is the recording's own truncation
// fact, threaded in by the caller.
BlameForest build_blame_forest(const std::vector<BlameAttr> &blame,
                               const DataflowStream &df,
                               const Projection &proj, bool truncated = false);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_BLAMEFOREST_H
