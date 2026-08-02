// taint.h — the forward-spread taint isochrone (57-causal-layers.md T3):
// from a chosen definition, how far and WHERE a value spreads across the
// plane, and whether it escapes into a different kind of region.
//
// Pure and engine-free (D4): the analysis/ slicer, the doc/ streams and the
// space/ models only — no GL, no ImGui, no engine — so it links into both
// desktop binaries and the null test harness.
//
// **THE AXIS.** The front advances on the DEF-USE GENERATION — first-reach BFS
// depth over the recorded `df_edge` graph — and never on the terrain-residency
// slice. Those are two different clocks that 34 deliberately left unfused, and
// a taint front that moved with `hud.t` would be claiming a temporal
// correspondence the recording does not state. `depth` is a hop count, not a
// time; `axis_note()` says so in the data so the HUD cannot forget to.
//
// **EXACT ONLY.** The input is a DataflowStream. A `SurveyEdge` — sampled,
// `exact:false` by construction — can never reach this builder, because the
// signature offers nowhere to put one. That is D7 invariant 1 made a property
// of the type rather than a rule someone has to remember.
#ifndef ASMDESK_SPACE_TAINT_H
#define ASMDESK_SPACE_TAINT_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/streams.h"
#include "space/projection.h"
#include "space/types.h"

namespace asmdesk::space {

// One reached step's mark on the plane: the MEMORY WRITE it made, at that
// write's own address. A step the walk reached that wrote nothing to memory
// produces no entry at all — it is reached, but it has no place, and inventing
// one would be the fabricated placement this layer most easily commits.
struct TaintReach {
    uint32_t step = 0;   // the reached def-use step
    int32_t depth = 0;   // FIRST-REACH BFS depth from the origin (hops, never
                         // a time)
    uint64_t addr = 0;   // the write's effective address (space == "abs")
    uint32_t cell = 0;   // its plane cell
    float u = 0, v = 0;
    Region::Kind kind = Region::Unknown;
    bool kind_known = false;
    // The flow was observed, the VALUE was not (`value_valid == false`): the
    // route is real and its content is unknown, so it renders HOLLOW rather
    // than solid — and never as an absence.
    bool hollow = false;
    // This cell's region kind differs from the origin's. THE LAYER'S ACTUAL
    // FINDING, and it is a comparison of two RECORDED region kinds — never an
    // inference about what the value "means".
    bool escape = false;
    // This step has an outgoing recorded edge to a step the recording never
    // DESCRIBED (`DataflowStream::has_step` is false for it). A POSITIVE mark
    // on a placeable cell, because "the value did not go further from here"
    // and "we did not look further from here" are different facts and both
    // render as nothing by default. This is the distinction T3 exists to
    // preserve.
    bool unknown_beyond = false;
};

struct TaintFront {
    std::vector<TaintReach> reached; // ascending by (depth, step)

    // The origin, and its region kind — the baseline every `escape` is
    // measured against. `origin_kind_known == false` (an origin that places
    // nowhere) disables escape marking entirely rather than comparing against
    // a default: an escape from an unknown kind is not a finding.
    bool has_origin = false;
    uint32_t origin_step = 0;
    uint32_t origin_cell = 0;
    Region::Kind origin_kind = Region::Unknown;
    bool origin_kind_known = false;

    int32_t depth_max = 0;
    // dt_walk::bounded — `max_depth` cut the walk off before the fixpoint, so
    // the rim is a LOWER BOUND and must render frayed, never as a boundary the
    // graph actually has.
    bool bounded = false;

    // --- what the front could NOT draw, each counted and stated ------------
    // A register-only write (`space == "reg"`) carries no address at all.
    // Colouring cell 0 for it is the archetypal fabricated placement, so it
    // tints nothing and is counted here instead.
    uint32_t reg_only_writes = 0;
    // A region-RELATIVE memory write (`space == "off"`). It is a real memory
    // write, but the offset is relative to a base the wire does not state for
    // DATA (a `df_step.rbase` is the CODE base — placing a data offset
    // against it would be a fabrication of a different kind). Counted and
    // stated, exactly as observed_data_spans already refuses to place these
    // raw (space/projection.h).
    uint32_t off_relative_writes = 0;
    // Reached steps the recording never described (`has_step` false). NOT
    // "not reached" — see TaintReach::unknown_beyond.
    uint32_t unknown_steps = 0;
    // Write addresses no region on the plane maps.
    uint32_t off_plane = 0;

    // The recording is truncated: every count here, and the front's extent
    // itself, is a stated LOWER BOUND. Carried so the caller reuses the slice
    // view's existing banner rather than inventing new wording.
    bool truncated = false;

    // Off unless there is a def-use graph and a valid origin to spread from.
    // `disabled_reason` is verbatim and never empty when `enabled` is false.
    bool enabled = false;
    std::string disabled_reason;

    // The axis disclaimer, carried in the DATA (converge.h's ConvergenceSet::
    // label() precedent) so no renderer can present the front as time.
    static const char *axis_note() {
        return "def-use generation (hops from the origin) — not time, and not "
               "the terrain playhead";
    }
    // The rim disclaimer, for a `bounded` walk.
    static const char *bounded_note() {
        return "the front was cut off at the depth limit — its rim is a lower "
               "bound, not a boundary the graph has";
    }
};

// Build the forward-spread front from `origin`, over `df`'s RECORDED def-use
// edges, placed on `proj`.
//
// Generation comes from `dt_walk_depth(edges, nsteps, origin, forward=true,
// max_depth)` (54 T5) — first-reach BFS depth — and NOT from
// `dt_slice_forward`, whose flat reachable set carries no depth at all
// (analysis/slice.h). `max_depth` is the caller's cap; pass a value >= nsteps
// for the unbounded walk.
//
// `truncated` is the recording's own truncation fact, threaded in by the
// caller (the builder does not re-derive it from a Recording it is not given).
TaintFront build_taint_front(const DataflowStream &df, const Projection &proj,
                             uint32_t origin, int32_t max_depth,
                             bool truncated = false);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_TAINT_H
