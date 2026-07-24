// take_view.h — the fork UX model (05-loom-day-one.md T6).
//
// Render a take beside the base fabric: the aligned prefix, patient zero, the
// cone of the edited fact, the interventional dim/hot verdicts, the dashed
// post-divergence tails and the fault card. PURE — no engine, no ImGui — so
// every verdict rule is a headless assertion. `forks.h` (which does link the
// engines) produces the take; this file only reasons about the result, which is
// why the render-only viewer can display a take somebody else recorded.
//
// THE THREE-WAY VERDICT is the heart of it, and its third state is the point:
//
//   dim      every matched write in the cone is value_valid on BOTH sides and
//            byte-equal — dependence without consequence, the most useful thing
//            a counterfactual can show.
//   hot      some matched write is valid on both sides and DIFFERENT.
//   neutral  either side never captured the value. Equality of unknown values
//            is never claimed, in either direction; the cone is outlined and
//            nothing is asserted about it.
//
// ALIGNMENT is shared-prefix over per-step `insn_off`, day-one — sound for
// same-entry runs, and it is not per-step semantic alignment (that is the
// Reweave growth rung). An argument edit on straight-line code diverges in
// VALUES, not in offsets: the gutter then says "aligned end-to-end" rather than
// inventing a patient zero that does not exist.
#ifndef ASMDESK_LOOM_TAKE_VIEW_H
#define ASMDESK_LOOM_TAKE_VIEW_H

#include <cstdint>
#include <string>
#include <vector>

#include "analysis/diff.h"
#include "loom/fabric_plan.h"

namespace asmdesk {

// A pure description of the ONE fact a take changed. `forks.h`'s loom_edit_t is
// the engine-side form; this is what the view needs and it drags in no engine
// header, which is what keeps this TU in the render-only binary.
struct loom_edit_desc_t {
    bool code_patch = false;
    // entry_arg: the folded container id of the argument register that changed
    // (rdi/rsi/rdx/rcx/r8/r9 — src/dataflow_emu.c:273's arg_regs, in order).
    uint32_t arg_reg = 0;
    std::string label; // "arg0 := 11", "code patch"
    // code_patch: the two byte images, so the cone can seed at the first step
    // whose instruction bytes actually differ.
    std::vector<uint8_t> base_code, take_code;
};

enum class take_verdict { neutral, dim, hot };
const char *take_verdict_name(take_verdict v);

struct loom_take_step_t {
    uint32_t step = 0;
    take_verdict verdict = take_verdict::neutral;
    bool in_cone = false;
    bool unaligned = false; // past the shared prefix
    bool no_writes = false; // in the cone but defines nothing: outline only
};

// The gutter node: one per take.
struct loom_take_node_t {
    std::string label;     // "(edited fact → new fabric)"
    std::string edit;      // the one fact
    std::string alignment; // "aligned end-to-end" | "patient zero at step N …"
    std::string fault;     // "" when the take did not fault
    std::string err;       // the take's loud failure, verbatim
    std::string disclosure; // kLoomForkDisclosure, restated in the panel
};

struct loom_take_view_t {
    uint32_t prefix = 0;        // shared-prefix length P
    dt_divergence div;          // 04's helper, over the two fabrics' insn_off
    std::vector<uint32_t> cone; // ascending steps in the cone of the edit
    std::vector<loom_take_step_t> steps; // one per BASE step
    loom_take_node_t node;
};

// Build the view. `err`/`fault` come from the take (forks.h fills them); they
// are passed in rather than read, so this stays engine-free.
loom_take_view_t
loom_take_view(const loom_fabric_t &base, const loom_fabric_t &take,
               const asmtest_defuse_edge_t *take_edges, size_t n_take_edges,
               const loom_edit_desc_t &edit, const std::string &fault,
               const std::string &err);

// Add the take's prims to a plan: patient zero, the dim/hot verdicts, the
// dashed tails and the fault card. Appends; never clears.
void loom_take_plan(const loom_take_view_t &v, const loom_view_t &cam,
                    std::vector<loom_prim_t> *out);

std::string loom_take_dump(const loom_take_view_t &v);

// Copy pinned by test_loom_forks.cpp.
extern const char *const kLoomAlignedEndToEnd;
extern const char *const kLoomDashedTailHover;

} // namespace asmdesk
#endif // ASMDESK_LOOM_TAKE_VIEW_H
