// stepplace.h — the ONE step->place conversion the causal layers share
// (57-causal-layers.md T1). Every layer in that brief (kernel-crossing spurs,
// the taint isochrone, the blame convergence forest, the dominant-path ridge)
// has a step or an event in hand and needs a *place*; each doing its own
// conversion would be four new chances to repeat the family's one documented
// mistake.
//
// **THE RULE THIS FILE EXISTS TO ENFORCE (46-3d-functional-roadmap.md §4).**
// Cross-axis brushing goes through the ADDRESS, never through an ordinal.
// `TrajPoint::t` (space/types.h) is a PER-TID VERTEX COUNTER — `p.t =
// next_t[tid]++` in build_trajectories (space/trajectory.cpp) — and is **not
// interchangeable with a dataflow step index**. They are both unsigned
// integers that both look like "when", and 46's G10 records the mismatch
// already shipped in the OTHER direction (`resolve_pick` sets `link.step =
// pv.t`, scene3d/pick.cpp). Nothing here ever inverts a place back through an
// ordinal: a step becomes an ADDRESS first, and only then a cell.
//
// **ADOPTED, NOT REBUILT.** 50-two-way-brushing.md T1 landed the
// address-first resolver as space/locate.h's `scene_locate_off` /
// `scene_locate_step` / `StepAddrResolver`, and this brief's own risk note
// says whichever landed first owns the mechanism and the second adopts it.
// 50 landed first, so `StepPlacer` is a THIN adapter over
// `space::StepAddrResolver`: it adds the plane coordinates, the region, and
// the running miss count that 57's HUD chip needs, and it re-derives NOTHING
// — the rbase/anchor resolution order, the anchor caching and every refusal
// reason come from locate.cpp verbatim.
//
// Pure and engine-free (D4): the standard library plus the space/ and doc/
// models — no GL, no ImGui, no engine — so it links into both desktop
// binaries and the null test harness.
#ifndef ASMDESK_SPACE_STEPPLACE_H
#define ASMDESK_SPACE_STEPPLACE_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/streams.h"
#include "space/locate.h"
#include "space/projection.h"

namespace asmdesk::space {

// Where a step (or a bare absolute address) sits on the plane, or why it does
// not. `placed == false` ALWAYS carries a non-empty `why` — a miss is a fact
// to state, never a silent drop — and every other field keeps its default. In
// particular `cell` stays 0 and MUST NOT be read: 0 is a real cell, and
// treating an unplaced step as cell 0 is the single most likely bug in this
// whole brief, which is why test_stepplace.cpp asserts against it by name.
//
// `why` is a std::string, not the brief's sketched `const char *`: the
// refusal reasons are the anchor's own, built at runtime
// (Anchor::reason / StepAddrResolver's fail_reason), so a `const char *`
// would dangle. Same choice space::Located::reason already made.
struct StepPlace {
    bool placed = false;
    uint64_t addr = 0;   // the resolved ABSOLUTE address
    float u = 0, v = 0;  // plane coordinates, valid iff placed
    uint32_t cell = 0;   // y*n + x (n = 2^order), valid iff placed
    const Region *region = nullptr; // the region holding `addr`; null iff !placed
    std::string why;     // when !placed: which resolution rung failed
};

// Place an already-ABSOLUTE address on the plane. This is the second half of
// every rung below (the first half turns a step into an address), factored out
// because three of this brief's four layers place addresses the recording
// states directly — a memory-write `ValRec::addr` (T3), a block start address
// (T5) — which have no step-resolution rung at all. Refuses (with a reason)
// when no region maps the address; never falls back to a cell.
//
// A caller placing addresses counts its OWN misses in its own layer struct
// (the `MispredLayer::off_plane` precedent), because "an address the plane
// does not map" and "a step whose base could not be resolved" are different
// failures and a single number would blur them.
StepPlace place_address(const Projection &proj, uint64_t addr);

// The shared step->place resolver: built ONCE per weave, reused by every
// layer, and it reports what it could not place.
//
// Constructed from (Projection, DataflowStream) rather than the brief's
// sketched (Projection, DataflowStream, Anchor): `StepAddrResolver` already
// derives and CACHES the anchor from `proj.regions` itself, and handing it a
// second, caller-supplied Anchor would create exactly the parallel source of
// truth this task exists to remove. The anchor is still resolved by
// `resolve_anchor()` — through locate.cpp, once.
//
// `at()` is non-const (the brief sketched it const) because a miss MUTATES
// the running count. That is the point: a placer whose misses were invisible
// to its own caller would be the silent-drop failure this task forbids.
class StepPlacer {
  public:
    StepPlacer(const Projection &proj, const DataflowStream &df)
        : proj_(proj), resolver_(proj, df), missed_(df.insn_off.size(), 0) {}

    // Resolve step `step` to a place, in the recording's OWN order — each
    // rung a stated fact rather than a fallback guess, and all of it
    // delegated to locate.h's StepAddrResolver:
    //   (a) `insn_rbase[step] != 0` -> rbase + off (37's on-the-wire region
    //       tag: a per-event fact that always wins);
    //   (b) otherwise the derived single-codeimage Anchor (36), which itself
    //       REFUSES rather than guessing when zero or >= 2 spans exist;
    //   (c) otherwise unplaced, with `why` naming the rung that failed.
    // A resolved address that no region maps is unplaced too, with its own
    // reason — the plane's refusal, not the anchor's.
    StepPlace at(uint32_t step);

    // How many DISTINCT steps `at()` refused so far — the HUD chip's "N steps
    // off-plane". Distinct, not a call counter: a layer that queries one
    // unplaceable step in two passes has one unplaceable step, and a chip that
    // said "2" would be a fabricated quantity. An out-of-range step index is
    // counted once too (it has no slot to dedupe against, so it is tallied
    // separately and added in).
    uint64_t unplaced() const { return distinct_missed_ + out_of_range_; }

    // The HUD line, in the wording placement_chips (scene3d/hud.h) already
    // established for this family — "N step(s) off-plane". Empty when nothing
    // was missed, so a caller can render it unconditionally and a clean weave
    // stays quiet.
    const std::string &note() const;

  private:
    const Projection &proj_;
    // No DataflowStream member: `resolver_` already holds the reference (and
    // the anchor cache), so a second one here would be the parallel source of
    // truth this adapter exists to avoid.
    StepAddrResolver resolver_;
    std::vector<char> missed_; // per step: already counted as unplaced?
    uint64_t distinct_missed_ = 0;
    uint64_t out_of_range_ = 0;
    mutable std::string note_;
    mutable uint64_t note_for_ = UINT64_MAX; // the count `note_` was built at
};

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_STEPPLACE_H
