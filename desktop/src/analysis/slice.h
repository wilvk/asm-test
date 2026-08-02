// slice.h — client-side def-use slicing over RECORDED edges
// (docs/internal/archive/gui/04-replay-views.md T1).
//
// Plan D5: replay slicing is computed in the viewer from the `df_edge` events a
// recording carries, never by calling back into the engine. That is what lets
// the render-only binary (asmtest-viewer, D4) slice a wrong value with zero
// engine dependencies — not even `asmtest_valtrace.h` is included here, so a
// test over these functions needs no repo headers at all.
//
// PARITY CONTRACT. The closure below must agree with the C slicer
// (src/dataflow.c `slice_dir`, :665) step for step, for every graph. The
// implementations differ only in cost — the C slicer rescans every edge per
// dequeued node (O(V*E)); this builds an adjacency list once and is O(V+E),
// which is what makes it usable at PT scale. `test_slice_diff` links the real
// `dataflow.o` and asserts the two agree over 200 pseudo-random graphs, so a
// divergence fails the build rather than showing a user a different slice in
// the GUI than the TUI shows.
#ifndef ASMDESK_ANALYSIS_SLICE_H
#define ASMDESK_ANALYSIS_SLICE_H

#include <cstdint>
#include <vector>

namespace asmdesk {

// One recorded def-use edge's endpoints (mirrors asmtest_defuse_edge_t; the
// carried `loc` is view chrome, not closure input, so it is not here).
struct dt_edge {
    uint32_t from_step = 0;
    uint32_t to_step = 0;
};

// The ascending, de-duplicated set of steps a closure reached.
struct dt_slice {
    std::vector<uint32_t> steps;
    bool contains(uint32_t step) const; // binary search over `steps`
};

// Forward: everything the value at `origin` influences. Backward: everything
// that produced it. `nsteps` is the step count the graph spans (the analogue of
// asmtest_defuse_t::nsteps) and BOUNDS the slice.
//
// The closure is the least set S such that:
//   (a) origin is in S if origin < nsteps — otherwise S is EMPTY. An
//       out-of-range origin yields {} and never {origin}: the step was not
//       recorded, so nothing about it is known.
//   (b) forward: for every edge (from -> to) with from in S and to < nsteps,
//       to is in S; backward: for every edge with to in S and from < nsteps,
//       from is in S; followed transitively to the fixed point.
// A valid origin with no edges yields {origin}, as does an empty edge set.
dt_slice dt_slice_forward(const std::vector<dt_edge> &edges, uint32_t nsteps,
                          uint32_t origin);
dt_slice dt_slice_backward(const std::vector<dt_edge> &edges, uint32_t nsteps,
                           uint32_t origin);

// 54-3d-catalog-phase0-plumbing.md T5: def-use GENERATION — how many hops from
// `origin` a step is — for a scene layer that wants to render "how far", not
// just "reachable". `dt_slice_forward`/`_backward` give the reachable set and
// throw the distance away; this keeps it.
struct dt_walk {
    std::vector<uint32_t> steps; // ascending, deduped — same set dt_slice gives
    std::vector<int32_t> depth;  // parallel to `steps`: first-reach BFS depth,
                                 // NEGATIVE for the backward direction (so a
                                 // caller walking both directions from one
                                 // origin can plot them on one signed axis)
    int32_t depth_min = 0, depth_max = 0; // the min/max of `depth`, not of the
                                           // unsigned hop count
    // The walk was cut off by `max_depth` before reaching the fixpoint: the
    // rim is a LOWER BOUND, not a boundary the graph actually has, and every
    // consumer must render it as one (frayed, not a hard edge). A field, not
    // an inference from `depth_max == max_depth`, because a graph whose TRUE
    // diameter from origin is exactly max_depth would make that inference
    // wrong.
    bool bounded = false;
};

// Same origin/nsteps contract as dt_slice_forward/backward: an out-of-range
// origin yields an EMPTY walk, never a single {origin} entry. `forward`
// selects direction exactly as it does for `closure()`'s callers above.
// `max_depth` (hop count, >= 0) caps how far the BFS expands; pass a value
// >= nsteps for the unbounded case (a visited-once BFS never needs more than
// nsteps-1 hops to reach every reachable step, so any such value gives the
// same `steps` as dt_slice_forward/backward — asserted in test_slice.cpp).
dt_walk dt_walk_depth(const std::vector<dt_edge> &edges, uint32_t nsteps,
                      uint32_t origin, bool forward, int32_t max_depth);

} // namespace asmdesk
#endif // ASMDESK_ANALYSIS_SLICE_H
