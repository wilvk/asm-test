// reweave_apply.cpp — engine leg for 42 T3. FULL BUILD ONLY (D4/D9), like
// forks.cpp: this TU calls loom_take_run_from_step (forks.h -> asmtest_emu.h),
// so it compiles only into the app tree (DESKTOP_LOOM_APP in mk/desktop.mk).
#include "loom/reweave_apply.h"

#include "loom/forks.h"

namespace asmdesk {

loom_take_view_t loom_run_reweave(const loom_fabric_t &base,
                                  const ReweaveSource &src, uint64_t req_step,
                                  const std::string &req_reg,
                                  uint64_t req_value) {
    loom_from_step_edit_t edit;
    edit.k = loom_from_step_edit_t::kind::reg;
    edit.step = req_step;
    edit.reg = req_reg;
    edit.reg_value = req_value;

    loom_edit_desc_t desc;
    desc.from_step_edit = true;
    desc.from_step = static_cast<uint32_t>(req_step);
    desc.label = edit.describe();

    loom_take_t take;
    if (!loom_take_run_from_step(src.code, src.code_len, src.args, src.nargs,
                                 edit, &take))
        // No fabric was ever built: pass an empty one through the SAME view
        // builder every other refusal uses, rather than hand-rolling a node —
        // loom_take_view already renders an err-only node faithfully.
        return loom_take_view(base, loom_fabric_t{}, nullptr, 0, desc, "",
                              take.err);

    loom_fabric_t take_fabric;
    std::string werr;
    if (!loom_fabric_build(take.vt(), take.g(), take.provenance(),
                           &take_fabric, &werr))
        return loom_take_view(base, loom_fabric_t{}, nullptr, 0, desc, "",
                              werr);

    // `take.err` is NOT a failure here (loom_take_run_from_step already
    // returned true): it is a real, loud CAVEAT alongside a real fabric — "the
    // reweave faulted after the edit" or "operand buffers filled: a lower
    // bound". Passing std::string() here would silently drop that notice
    // exactly like an unasserted engine warning; take.err must reach the
    // gutter verbatim (D7). `take.fault_card()` now renders REAL fault detail
    // (kind / address / disassembly at the faulting rip) whenever the reweave
    // actually faulted: loom_take_run_from_step populates `result` from the
    // resume seam's emu_result_t (src/dataflow_resume.c:
    // asmtest_dataflow_emu_run_from_current's result_out), closing the gap
    // docs/internal/archive/gui/42-loom-reweave-consumption.md's status banner
    // recorded. `fault_card()` itself stays empty for a clean (non-faulting)
    // reweave — it checks result.faulted before rendering anything.
    return loom_take_view(base, take_fabric, take.edges.data(),
                          take.edges.size(), desc, take.fault_card(),
                          take.err);
}

} // namespace asmdesk
