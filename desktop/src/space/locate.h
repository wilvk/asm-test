// locate.h — the pure selection->cell resolver of 50-two-way-brushing.md T1:
// "where on the plane is the thing the flat views have brushed?", or why not.
//
// A SEPARATE TU from projection.h/goto.h because this answers a different
// question again — goto.h points a camera at a NAMED destination (an address
// typed in, a region chosen from a list); this resolves the app's shared
// Selection (ui/selection.h) — a code OFFSET or a dataflow STEP, in the
// recording's own basis — onto the same plane. Pure math over the space/ and
// doc/ models: no GL, no ImGui, no engine (D4), so it is exercised headlessly
// and links into both desktop binaries and the null test harness.
#ifndef ASMDESK_SPACE_LOCATE_H
#define ASMDESK_SPACE_LOCATE_H

#include <cstdint>
#include <string>

#include "doc/recording.h"
#include "doc/streams.h"
#include "space/projection.h"

namespace asmdesk::space {

// The answer to "where is this brushed entity, or why can this brief not say".
// `reason` is verbatim and NEVER empty when `ok` is false — the fidelity floor
// this brief states throughout: an unplaceable selection reads as "unplaceable,
// because X", never as silently absent. `ambiguous` does not affect `ok`: the
// cell is still the correct answer to "where", it is simply not the only step
// that reaches it.
struct Located {
    bool ok = false;
    uint32_t cell = 0;      // plane cell, valid iff ok
    uint64_t addr = 0;      // the absolute address it resolved through
    std::string how;        // "offset" | "step->offset (wire rbase)" |
                            // "step->offset (derived anchor)" — which route,
                            // and for the step route, which base it trusted
    std::string reason;     // why not, when !ok (verbatim, never empty)
    bool ambiguous = false; // the address is real but not uniquely the target
};

// Resolve Selection::off — a code offset IN THE RECORDING'S BASIS — to a plane
// cell. The basis is read off the recording itself (the first `trace` event's
// stated "basis"; a recording with no `trace` but a `df_step` stream is "rel"
// by construction, matching build_trajectories' own fallback rule) rather than
// taken on faith from the caller, so a caller need not have already built a
// TrajectorySet just to ask this question.
//
// An "abs" basis takes `off` as the address directly. A "rel" basis routes
// through resolve_anchor()/Anchor::place() — the SAME derivation 36/37 use for
// the trajectory's own PC path — and carries the anchor's `reason` verbatim on
// refusal. A recording with neither a `trace` nor a `df_step` stream, or one
// whose lone `trace` event states no basis, refuses: there is nothing to
// resolve an offset against.
//
// `ambiguous` is set when another event in the SAME stream (`trace` if that is
// where the basis came from, else `df_step`) carries the identical raw `off` —
// a loop body revisits one address at many steps, and the cell is where, not
// when.
Located scene_locate_off(const Projection &proj, const Recording &rec,
                         uint64_t off);

// Resolve Selection::step — a dataflow-pass-local step index — to a plane
// cell, through the SAME pass the selection belongs to (`df` must already be
// the caller's chosen pass — Streams::df, or an explicit SegmentedDataflow
// entry; this function never re-derives which pass "step" means). Out of
// range, or a step the pass did not cover, refuses. When the step's own
// `insn_rbase` states a base (37: a per-event, wire-stated fact), that wins;
// otherwise falls back to resolve_anchor()/Anchor::place() exactly as
// scene_locate_off's rel route does, labelled distinctly in `how` so a
// wire-stated base and a derived one are never presented as the same claim
// (the distinction TrajectorySet::anchor_source already draws).
//
// `ambiguous` is set when another step of THIS pass resolves to the same
// address (a loop body).
Located scene_locate_step(const Projection &proj, const DataflowStream &df,
                          uint32_t step);

// The per-step address resolution scene_locate_step performs, factored out so
// a caller who needs to resolve MANY steps (pick.cpp's reverse-direction
// vertex->step search, 50-two-way-brushing.md T3: "search DataflowStream::
// insn_off for a step at that address") pays for one Anchor lookup total,
// not one per step queried — the same anchor-caching discipline
// scene_locate_step already applies to its own internal ambiguity scan, now
// reusable across an external scan too. One instance per `df`; no state
// beyond the cached Anchor, so a caller within a single resolve_pick() call
// constructs and discards one.
class StepAddrResolver {
  public:
    StepAddrResolver(const Projection &proj, const DataflowStream &df)
        : proj_(proj), df_(df) {}

    // Resolve step `i`'s own offset to an absolute address. False when `i` is
    // out of range, or the step has no wire rbase and no span will anchor it
    // (`how`, when non-null, is left untouched on failure).
    bool resolve(uint32_t i, uint64_t *addr, std::string *how = nullptr);

  private:
    const Projection &proj_;
    const DataflowStream &df_;
    bool anchor_computed_ = false;
    Anchor anchor_;
};

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_LOCATE_H
