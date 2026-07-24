// lineage.h — selection IS lineage (05-loom-day-one.md T3).
//
// Click any (lane, step): the value resident there lights as ONE thread-tree —
// everything that produced it and everything it goes on to touch — with a
// generation number per step so `[` and `]` walk the tree a ring at a time. A
// biography narrates the same facts in sentences, and the zeroization audit
// answers the question a crypto reviewer actually has: "at time T, who still
// holds something descended from this secret?"
//
// TWO HONESTY RULES, both structural:
//
//  1. A born-of-untraced-state worldline has NO in-window ancestry. The
//     selection says so and returns an empty closure rather than seeding the
//     walk at step 0, which would attribute the value to whatever ran first.
//  2. The audit is bounded by the traced window, and its own title says so.
//     "Clear" here means *not overwritten inside what was recorded* — it is not
//     a statement about the process, and the copy never lets it read as one.
//
// The closure is the same relation 04's dt_slice_forward/_backward compute (and
// the same one src/dataflow.c's slicer computes); this adds the BFS DEPTH that
// the generation walk needs, and test_loom_parity pins it against both.
#ifndef ASMDESK_LOOM_LINEAGE_H
#define ASMDESK_LOOM_LINEAGE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "loom/fabric.h"

namespace asmdesk {

struct loom_selection_t {
    uint32_t origin_step = 0;
    uint32_t origin_span = kLoomNoSpan;
    std::vector<uint32_t> steps; // ascending, deduped — the whole thread-tree
    std::vector<int> generation; // parallel to `steps`; negative = ancestors
    int gen_lo = 0, gen_hi = 0;  // the walk's bounds
    int gen_view = 0;            // [ / ] move this
    // The selected worldline has no producer inside the recorded window. Its
    // closure is empty and the UI must say why rather than showing "nothing
    // found", which reads as "nothing happened".
    bool born_untraced = false;
    std::string note;
};

// Select the value resident on `lane` at `step`. False when the lane index is
// out of range or nothing is resident there (a lane the trace has not reached
// yet is not the same as a lane holding zero).
bool loom_select(const loom_fabric_t &f, const asmtest_defuse_edge_t *e,
                 size_t n, uint32_t lane, uint32_t step, loom_selection_t *out);

// The steps at exactly one generation — what `[` / `]` light.
std::vector<uint32_t> loom_generation(const loom_selection_t &sel, int gen);
// Move gen_view one ring out (`[`, toward ancestors) or in (`]`), clamped.
void loom_gen_step(loom_selection_t &sel, int delta);

// --- biography --------------------------------------------------------------

struct loom_bio_row_t {
    std::string kind; // "birth" | "hop" | "escape" | "producer" | "window"
    std::string text;
};

// Narrate one span: how the value was born, every hop it takes (disassembled
// from prov.disasm, or the bare offset — D10), where it escapes to memory, what
// tier produced it, and what the recorded window does and does not cover.
std::vector<loom_bio_row_t> loom_biography(const loom_fabric_t &f,
                                           const asmtest_defuse_edge_t *e,
                                           size_t n, uint32_t span);

// --- zeroization audit ------------------------------------------------------

// One lane still holding a descendant of the audited birth at the playhead.
//
// DOC DEVIATION (2026-07-24). 05-T3 gives the signature
// `std::vector<uint32_t> *lit_lanes` and two sentences later requires that
// "hollow residents light with the hollow flag (route known, value not)" — a
// bare lane index cannot carry that flag. The richer row is the shipped form;
// `loom_audit_lanes` below keeps the doc's plain-index call site working.
struct loom_lit_t {
    uint32_t lane = 0;
    uint32_t span = kLoomNoSpan;
    bool hollow = false; // the resident value was never captured
};

// Light every lane whose RESIDENT span at `playhead` was defined by a step in
// the forward closure of `birth_step`. Scrubbing the playhead re-evaluates
// residency only — the closure is computed once.
//
// A born-of-untraced-state span never lights: its provenance starts at
// instrumentation, so the fabric cannot claim it descends from the birth.
size_t loom_audit(const loom_fabric_t &f, const asmtest_defuse_edge_t *e,
                  size_t n, uint32_t birth_step, uint32_t playhead,
                  std::vector<loom_lit_t> *lit);
size_t loom_audit_lanes(const loom_fabric_t &f, const asmtest_defuse_edge_t *e,
                        size_t n, uint32_t birth_step, uint32_t playhead,
                        std::vector<uint32_t> *lit_lanes);

// The audit's copy, pinned verbatim by test_loom_lineage.cpp.
extern const char *const kLoomAuditTitle;
extern const char *const kLoomAuditHover;

} // namespace asmdesk
#endif // ASMDESK_LOOM_LINEAGE_H
